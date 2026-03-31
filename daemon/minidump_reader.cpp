// daemon/minidump_reader.cpp — Breakpad minidump processor wrapper

#include "daemon/minidump_reader.h"

#include <sys/stat.h>
#include <array>
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
#include "google_breakpad/common/minidump_cpu_arm64.h"
#include "google_breakpad/common/minidump_format.h"

namespace crashomon {
namespace {

std::string FormatTimestamp(uint32_t time_date_stamp) {
  const auto unix_ts = static_cast<time_t>(time_date_stamp);
  struct tm tm_buf{};
  gmtime_r(&unix_ts, &tm_buf);
  std::array<char, 32> buf{};
  // NOLINTNEXTLINE(cert-err33-c)
  (void)strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
  return buf.data();
}

std::string Basename(const std::string& path) {
  const auto pos = path.rfind('/');
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// Collect AMD64 GPRs in tombstone display order.
std::vector<std::pair<std::string, uint64_t>> ExtractAMD64Regs(
    const MDRawContextAMD64& ctx) {
  return {
    {"rax", ctx.rax}, {"rbx", ctx.rbx}, {"rcx", ctx.rcx},
    {"rdx", ctx.rdx}, {"rsi", ctx.rsi}, {"rdi", ctx.rdi},
    {"rbp", ctx.rbp}, {"rsp", ctx.rsp}, {"r8",  ctx.r8},
    {"r9",  ctx.r9},  {"r10", ctx.r10}, {"r11", ctx.r11},
    {"r12", ctx.r12}, {"r13", ctx.r13}, {"r14", ctx.r14},
    {"r15", ctx.r15}, {"rip", ctx.rip},
  };
}

// Collect ARM64 GPRs in tombstone display order.
// iregs layout: x0-x28 (indices 0-28), fp/x29 (29), lr/x30 (30), sp (31), pc (32).
std::vector<std::pair<std::string, uint64_t>> ExtractARM64Regs(
    const MDRawContextARM64& ctx) {
  std::vector<std::pair<std::string, uint64_t>> regs;
  regs.reserve(33);
  for (int idx = 0; idx <= 28; ++idx) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    regs.emplace_back("x" + std::to_string(idx), ctx.iregs[static_cast<size_t>(idx)]);
  }
  regs.emplace_back("fp", ctx.iregs[MD_CONTEXT_ARM64_REG_FP]);
  regs.emplace_back("lr", ctx.iregs[MD_CONTEXT_ARM64_REG_LR]);
  regs.emplace_back("sp", ctx.iregs[MD_CONTEXT_ARM64_REG_SP]);
  regs.emplace_back("pc", ctx.iregs[MD_CONTEXT_ARM64_REG_PC]);
  return regs;
}

void CollectModules(const google_breakpad::ProcessState& process_state,
                    MinidumpInfo& info) {
  const auto* modules = process_state.modules();
  if (modules == nullptr) { return; }
  const unsigned int module_count = modules->module_count();
  info.modules.reserve(module_count);
  for (unsigned int idx = 0; idx < module_count; ++idx) {
    const auto* mod = modules->GetModuleAtIndex(idx);
    if (mod == nullptr) { continue; }
    info.modules.push_back({mod->code_file(), mod->base_address(), mod->size(),
                             mod->debug_identifier()});
  }
}

ThreadInfo BuildThread(
    int thread_idx, bool is_crashing,
    const std::vector<google_breakpad::CallStack*>& threads,
    const std::vector<std::string>* thread_names) {
  ThreadInfo thread_info;
  const auto* stack = threads[static_cast<size_t>(thread_idx)];
  thread_info.tid = stack->tid();
  thread_info.is_crashing = is_crashing;

  if (thread_names != nullptr &&
      static_cast<size_t>(thread_idx) < thread_names->size()) {
    thread_info.name = (*thread_names)[static_cast<size_t>(thread_idx)];
  }

  const auto* frames = stack->frames();
  if (frames != nullptr) {
    thread_info.frames.reserve(frames->size());
    for (const auto* frame : *frames) {
      if (frame == nullptr) { continue; }
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

void CollectThreads(const google_breakpad::ProcessState& process_state,
                    MinidumpInfo& info) {
  const auto* threads = process_state.threads();
  if (threads == nullptr) { return; }
  const auto* thread_names = process_state.thread_names();
  const int requesting = process_state.requesting_thread();

  if (requesting >= 0 &&
      static_cast<size_t>(requesting) < threads->size() &&
      (*threads)[static_cast<size_t>(requesting)] != nullptr) {
    info.threads.push_back(
        BuildThread(requesting, /*is_crashing=*/true, *threads, thread_names));
  }
  for (int idx = 0; idx < static_cast<int>(threads->size()); ++idx) {
    if (idx == requesting) { continue; }
    if ((*threads)[static_cast<size_t>(idx)] == nullptr) { continue; }
    info.threads.push_back(
        BuildThread(idx, /*is_crashing=*/false, *threads, thread_names));
  }
}

void ExtractRegisters(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  if (info.threads.empty() || !info.threads[0].is_crashing) { return; }
  auto* thread_list = raw.GetThreadList();
  if (thread_list == nullptr) { return; }
  auto* raw_thread = thread_list->GetThreadByID(info.crashing_tid);
  if (raw_thread == nullptr) { return; }
  auto* ctx = raw_thread->GetContext();
  if (ctx == nullptr) { return; }
  if (const auto* amd64 = ctx->GetContextAMD64()) {
    info.threads[0].registers = ExtractAMD64Regs(*amd64);
  } else if (const auto* arm64 = ctx->GetContextARM64()) {
    info.threads[0].registers = ExtractARM64Regs(*arm64);
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
    return absl::InternalError(
        std::string("No thread list in minidump: ") + path);
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
