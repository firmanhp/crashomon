// daemon/minidump_reader.cpp — Breakpad minidump processor wrapper

#include "daemon/minidump_reader.h"

#include <sys/stat.h>
#include <ctime>
#include <string>
#include <vector>

#include "absl/status/status.h"
// Processor headers pull in minidump_format.h → breakpad_types.h (defines
// uint128_struct). Include them BEFORE minidump_cpu_amd64.h which uses that type.
#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/code_modules.h"
#include "google_breakpad/processor/minidump.h"
#include "google_breakpad/processor/minidump_processor.h"
#include "google_breakpad/processor/process_result.h"
#include "google_breakpad/processor/process_state.h"
#include "google_breakpad/processor/stack_frame.h"
// CPU-specific context structs — must come after breakpad_types.h (above).
#include "google_breakpad/common/minidump_cpu_amd64.h"
#include "google_breakpad/common/minidump_format.h"

namespace crashomon {
namespace {

std::string FormatTimestamp(uint32_t time_date_stamp) {
  time_t t = static_cast<time_t>(time_date_stamp);
  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
  return buf;
}

std::string Basename(const std::string& path) {
  auto pos = path.rfind('/');
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// Collect AMD64 GPRs in tombstone display order.
std::vector<std::pair<std::string, uint64_t>> ExtractAMD64Regs(
    const MDRawContextAMD64& c) {
  return {
    {"rax", c.rax}, {"rbx", c.rbx}, {"rcx", c.rcx},
    {"rdx", c.rdx}, {"rsi", c.rsi}, {"rdi", c.rdi},
    {"rbp", c.rbp}, {"rsp", c.rsp}, {"r8",  c.r8},
    {"r9",  c.r9},  {"r10", c.r10}, {"r11", c.r11},
    {"r12", c.r12}, {"r13", c.r13}, {"r14", c.r14},
    {"r15", c.r15}, {"rip", c.rip},
  };
}

}  // namespace

absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path) {
  // High-level processing via MinidumpProcessor: gives us crash reason,
  // stack frames, and module list without needing symbols.
  google_breakpad::MinidumpProcessor processor(nullptr, nullptr);
  google_breakpad::ProcessState process_state;
  google_breakpad::ProcessResult result = processor.Process(path, &process_state);

  if (result != google_breakpad::PROCESS_OK) {
    return absl::InternalError(
        std::string("MinidumpProcessor failed for '") + path +
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
    return absl::InternalError(
        std::string("Failed to re-read minidump: ") + path);
  }

  auto* misc = raw.GetMiscInfo();
  if (misc && misc->misc_info() &&
      (misc->misc_info()->flags1 & MD_MISCINFO_FLAGS1_PROCESS_ID)) {
    info.pid = misc->misc_info()->process_id;
  }

  // Collect modules from ProcessState (already parsed + sorted).
  const auto* modules = process_state.modules();
  if (modules) {
    unsigned int n = modules->module_count();
    info.modules.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
      const auto* m = modules->GetModuleAtIndex(i);
      if (!m) continue;
      info.modules.push_back({m->code_file(), m->base_address(), m->size(),
                               m->debug_identifier()});
    }
  }

  // Process name = basename of the first (lowest-address) module,
  // which is the main executable on Linux.
  if (!info.modules.empty()) {
    info.process_name = Basename(info.modules[0].path);
  }

  // Collect threads — crashing thread first.
  const auto* threads = process_state.threads();
  const auto* thread_names = process_state.thread_names();
  int requesting = process_state.requesting_thread();

  if (!threads) {
    return absl::InternalError(
        std::string("No thread list in minidump: ") + path);
  }

  auto populate_thread = [&](int idx, bool is_crashing) {
    if (idx < 0 || static_cast<size_t>(idx) >= threads->size()) return;
    const auto* stack = (*threads)[static_cast<size_t>(idx)];
    if (!stack) return;

    ThreadInfo ti;
    ti.tid = stack->tid();
    ti.is_crashing = is_crashing;

    if (thread_names &&
        static_cast<size_t>(idx) < thread_names->size()) {
      ti.name = (*thread_names)[static_cast<size_t>(idx)];
    }

    const auto* frames = stack->frames();
    if (frames) {
      ti.frames.reserve(frames->size());
      for (const auto* f : *frames) {
        if (!f) continue;
        FrameInfo fi;
        fi.pc = f->instruction;
        fi.module_offset = 0;
        if (f->module) {
          fi.module_path = f->module->code_file();
          fi.module_offset = f->instruction - f->module->base_address();
        }
        ti.frames.push_back(fi);
      }
    }
    info.threads.push_back(std::move(ti));
  };

  if (requesting >= 0) {
    populate_thread(requesting, /*is_crashing=*/true);
  }
  for (int i = 0; i < static_cast<int>(threads->size()); ++i) {
    if (i != requesting) populate_thread(i, /*is_crashing=*/false);
  }

  // Crashing TID.
  if (!info.threads.empty() && info.threads[0].is_crashing) {
    info.crashing_tid = info.threads[0].tid;
  }

  // Extract AMD64 registers for the crashing thread.
  if (!info.threads.empty() && info.threads[0].is_crashing) {
    auto* thread_list = raw.GetThreadList();
    if (thread_list) {
      auto* raw_thread = thread_list->GetThreadByID(info.crashing_tid);
      if (raw_thread) {
        auto* ctx = raw_thread->GetContext();
        if (ctx) {
          const auto* amd64 = ctx->GetContextAMD64();
          if (amd64) {
            info.threads[0].registers = ExtractAMD64Regs(*amd64);
          }
        }
      }
    }
  }

  return info;
}

}  // namespace crashomon
