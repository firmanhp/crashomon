// tombstone/minidump_reader.cpp — Breakpad minidump processor wrapper

#include "tombstone/minidump_reader.h"

#include <sys/stat.h>  // NOLINT(misc-include-cleaner) — pulled in for struct stat; transitively available but explicitly included for clarity.

#include <array>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "google_breakpad/common/minidump_format.h"
#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/code_modules.h"
#include "google_breakpad/processor/minidump.h"
#include "google_breakpad/processor/minidump_processor.h"
#include "google_breakpad/processor/process_result.h"
#include "google_breakpad/processor/process_state.h"
#include "google_breakpad/processor/stack_frame.h"
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

void CollectModules(const google_breakpad::ProcessState& process_state, MinidumpInfo& info) {
  const auto* modules = process_state.modules();
  if (modules == nullptr) {
    return;
  }
  const unsigned int module_count = modules->module_count();
  info.modules.reserve(module_count);
  for (unsigned int idx = 0; idx < module_count; ++idx) {
    const auto* mod = modules->GetModuleAtIndex(idx);
    if (mod == nullptr) {
      continue;
    }
    info.modules.push_back({.path = mod->code_file(),
                            .base_address = mod->base_address(),
                            .size = mod->size(),
                            .build_id = mod->debug_identifier()});
  }
}

ThreadInfo BuildThread(int thread_idx, bool is_crashing,
                       const std::vector<google_breakpad::CallStack*>& threads,
                       const std::vector<std::string>* thread_names) {
  ThreadInfo thread_info;
  const auto* stack = threads[static_cast<size_t>(thread_idx)];
  thread_info.tid = stack->tid();
  thread_info.is_crashing = is_crashing;

  if (thread_names != nullptr && static_cast<size_t>(thread_idx) < thread_names->size()) {
    thread_info.name = (*thread_names)[static_cast<size_t>(thread_idx)];
  }

  const auto* frames = stack->frames();
  if (frames != nullptr) {
    thread_info.frames.reserve(frames->size());
    for (const auto* frame : *frames) {
      if (frame == nullptr) {
        continue;
      }
      FrameInfo frame_info;
      frame_info.pc = frame->instruction;
      frame_info.module_offset = 0;
      if (frame->module != nullptr) {
        frame_info.module_path = frame->module->code_file();
        frame_info.module_offset = frame->instruction - frame->module->base_address();
      }
      thread_info.frames.push_back(frame_info);
    }
  }
  return thread_info;
}

void CollectThreads(const google_breakpad::ProcessState& process_state, MinidumpInfo& info) {
  const auto* threads = process_state.threads();
  if (threads == nullptr) {
    return;
  }
  const auto* thread_names = process_state.thread_names();
  const int requesting = process_state.requesting_thread();

  if (requesting >= 0 && static_cast<size_t>(requesting) < threads->size() &&
      (*threads)[static_cast<size_t>(requesting)] != nullptr) {
    info.threads.push_back(BuildThread(requesting, /*is_crashing=*/true, *threads, thread_names));
  }
  for (int idx = 0; idx < static_cast<int>(threads->size()); ++idx) {
    if (idx == requesting) {
      continue;
    }
    if ((*threads)[static_cast<size_t>(idx)] == nullptr) {
      continue;
    }
    info.threads.push_back(BuildThread(idx, /*is_crashing=*/false, *threads, thread_names));
  }
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

}  // namespace

absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path) {
  // High-level processing via MinidumpProcessor: gives us crash reason,
  // stack frames, and module list without needing symbols.
  google_breakpad::MinidumpProcessor processor(nullptr, nullptr);
  google_breakpad::ProcessState process_state;
  const google_breakpad::ProcessResult result = processor.Process(path, &process_state);

  if (result != google_breakpad::PROCESS_OK) {
    return absl::InternalError(std::string("MinidumpProcessor failed for '") + path +
                               "' (result=" + std::to_string(static_cast<int>(result)) + ")");
  }

  MinidumpInfo info;
  info.minidump_path = path;
  info.timestamp = FormatTimestamp(process_state.time_date_stamp());
  info.fault_addr = process_state.crash_address();
  info.signal_info = process_state.crash_reason();

  if (const auto* exc = process_state.exception_record()) {
    info.signal_number = exc->code();
    info.signal_code = exc->flags();
  }

  // Low-level pass: PID from misc info, registers from thread context.
  // We open a second Minidump instance rather than mutating the processor's.
  google_breakpad::Minidump raw(path);
  if (!raw.Read()) {
    return absl::InternalError(std::string("Failed to re-read minidump: ") + path);
  }

  auto* misc = raw.GetMiscInfo();
  if (misc != nullptr && misc->misc_info() != nullptr &&
      ((misc->misc_info()->flags1 & MD_MISCINFO_FLAGS1_PROCESS_ID) != 0U)) {
    info.pid = misc->misc_info()->process_id;
  }

  CollectModules(process_state, info);

  // Process name = basename of the first (lowest-address) module,
  // which is the main executable on Linux.
  if (!info.modules.empty()) {
    info.process_name = Basename(info.modules[0].path);
  }

  if (process_state.threads() == nullptr) {
    return absl::InternalError(std::string("No thread list in minidump: ") + path);
  }

  CollectThreads(process_state, info);

  // Crashing TID.
  if (!info.threads.empty() && info.threads[0].is_crashing) {
    info.crashing_tid = info.threads[0].tid;
  }

  ExtractRegisters(raw, info);

  return info;
}

}  // namespace crashomon
