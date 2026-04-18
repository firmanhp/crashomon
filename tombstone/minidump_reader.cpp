// tombstone/minidump_reader.cpp — low-level minidump reader (no stack walking)

#include "tombstone/minidump_reader.h"

#include <sys/stat.h>  // NOLINT(misc-include-cleaner) — pulled in for struct stat; transitively available but explicitly included for clarity.

#include <array>
#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "google_breakpad/common/minidump_format.h"
#include "google_breakpad/processor/minidump.h"
#include "tombstone/register_extract.h"

namespace crashomon {
namespace {

std::string FormatTimestamp(uint32_t time_date_stamp) {
  const auto unix_ts = static_cast<time_t>(time_date_stamp);
  struct tm tm_buf {};
  gmtime_r(&unix_ts, &tm_buf);
  constexpr size_t timestamp_buf_size = 32;
  std::array<char, timestamp_buf_size> buf{};
  if (strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm_buf) == 0) {
    return "1970-01-01T00:00:00Z";
  }
  return buf.data();
}

std::string Basename(const std::string& path) {
  const auto pos = path.rfind('/');
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

void ExtractExceptionInfo(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  auto* exc = raw.GetException();
  if (exc == nullptr) {
    return;
  }
  const auto* rec = exc->exception();
  if (rec == nullptr) {
    return;
  }

  info.signal_number = rec->exception_record.exception_code;
  info.signal_code = rec->exception_record.exception_flags;
  info.fault_addr = rec->exception_record.exception_address;
  info.crashing_tid = rec->thread_id;

  // Build a signal_info string in the "SIGNAME / CODENAME" format expected by
  // SplitSignalInfo in tombstone_formatter.cpp.
  static const std::unordered_map<uint32_t, const char*> kSigNames = {
      {4, "SIGILL"}, {6, "SIGABRT"}, {7, "SIGBUS"}, {8, "SIGFPE"}, {11, "SIGSEGV"},
  };
  const auto sig_it = kSigNames.find(info.signal_number);
  const std::string sig_name = (sig_it != kSigNames.end())
                                   ? std::string(sig_it->second)
                                   : absl::StrCat("signal ", info.signal_number);

  // Code names for the most common SIGSEGV si_codes on Linux.
  static const std::unordered_map<uint32_t, const char*> kSegvCodes = {
      {1, "SEGV_MAPERR"},
      {2, "SEGV_ACCERR"},
  };
  constexpr uint32_t sigsegv = 11;
  if (info.signal_number == sigsegv && info.signal_code != 0) {
    const auto code_it = kSegvCodes.find(info.signal_code);
    if (code_it != kSegvCodes.end()) {
      info.signal_info = absl::StrCat(sig_name, " / ", code_it->second);
      return;
    }
  }
  info.signal_info = sig_name;
}

void ExtractModules(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  auto* module_list = raw.GetModuleList();
  if (module_list == nullptr) {
    return;
  }
  const unsigned int count = module_list->module_count();
  info.modules.reserve(count);
  for (unsigned int i = 0; i < count; ++i) {
    const auto* mod = module_list->GetModuleAtIndex(i);
    if (mod == nullptr) {
      continue;
    }
    info.modules.push_back({
        .path = mod->code_file(),
        .base_address = mod->base_address(),
        .size = mod->size(),
        .build_id = mod->code_identifier(),
    });
  }
}

// Returns a FrameInfo for the given program_counter by looking up the containing module.
// If no module covers program_counter, returns a FrameInfo with empty module_path (rendered as ???).
FrameInfo MakeFrameFromPC(uint64_t program_counter, const std::vector<ModuleInfo>& modules) {
  FrameInfo frame;
  frame.pc = program_counter;
  for (const auto& mod : modules) {
    if (program_counter >= mod.base_address && program_counter < mod.base_address + mod.size) {
      frame.module_path = mod.path;
      frame.module_offset = program_counter - mod.base_address;
      frame.build_id = mod.build_id;
      return frame;
    }
  }
  return frame;
}

void ExtractRegisters(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  if (info.threads.empty() || !info.threads[0].is_crashing) {
    return;
  }
  auto* thread_list = raw.GetThreadList();
  if (thread_list == nullptr) {
    return;
  }
  auto* raw_thread = thread_list->GetThreadByID(info.crashing_tid);
  if (raw_thread == nullptr) {
    return;
  }
  auto* ctx = raw_thread->GetContext();
  if (ctx == nullptr) {
    return;
  }
  switch (ctx->GetContextCPU()) {
    case MD_CONTEXT_AMD64:
      if (const auto* amd64 = ctx->GetContextAMD64()) {
        info.threads[0].registers = ExtractAMD64Regs(*amd64);
      }
      break;
    case MD_CONTEXT_ARM64:
      if (const auto* arm64 = ctx->GetContextARM64()) {
        info.threads[0].registers = ExtractARM64Regs(*arm64);
      }
      break;
    default:
      break;
  }
}

void ExtractThreadName(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  if (info.threads.empty()) {
    return;
  }
  auto* name_list = raw.GetThreadNameList();
  if (name_list == nullptr) {
    return;
  }
  const unsigned int count = name_list->thread_name_count();
  for (unsigned int i = 0; i < count; ++i) {
    const auto* entry = name_list->GetThreadNameAtIndex(i);
    if (entry == nullptr) {
      continue;
    }
    uint32_t tid = 0;
    if (!entry->GetThreadID(&tid)) {
      continue;
    }
    if (tid == info.crashing_tid) {
      info.threads[0].name = entry->GetThreadName();
      return;
    }
  }
}

}  // namespace

absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path) {
  google_breakpad::Minidump raw(path);
  if (!raw.Read()) {
    return absl::InternalError(absl::StrCat("Failed to read minidump: ", path));
  }

  MinidumpInfo info;
  info.minidump_path = path;

  // Timestamp from the minidump header.
  if (const auto* hdr = raw.header()) {
    info.timestamp = FormatTimestamp(hdr->time_date_stamp);
  }

  // PID from misc info.
  if (auto* misc = raw.GetMiscInfo();
      misc != nullptr && misc->misc_info() != nullptr &&
      ((misc->misc_info()->flags1 & MD_MISCINFO_FLAGS1_PROCESS_ID) != 0U)) {
    info.pid = misc->misc_info()->process_id;
  }

  ExtractExceptionInfo(raw, info);
  ExtractModules(raw, info);

  // Process name = basename of the lowest-address module (main executable on Linux).
  if (!info.modules.empty()) {
    info.process_name = Basename(info.modules[0].path);
  }

  // Build the single crashing-thread entry, then populate registers and the PC frame.
  if (info.crashing_tid != 0) {
    ThreadInfo thread;
    thread.tid = info.crashing_tid;
    thread.is_crashing = true;
    info.threads.push_back(std::move(thread));

    ExtractRegisters(raw, info);

    // Derive the PC from the register context (by name: "pc" for ARM64, "rip" for AMD64).
    uint64_t program_counter = 0;
    for (const auto& [name, val] : info.threads[0].registers) {
      if (name == "pc" || name == "rip") {
        program_counter = val;
        break;
      }
    }
    if (program_counter != 0) {
      info.threads[0].frames.push_back(MakeFrameFromPC(program_counter, info.modules));
    }

    ExtractThreadName(raw, info);
  }

  return info;
}

}  // namespace crashomon
