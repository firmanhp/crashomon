// tombstone/tombstone_formatter.cpp — crashomon tombstone formatter

#include "tombstone/tombstone_formatter.h"

#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "tombstone/minidump_reader.h"

namespace crashomon {
namespace {

constexpr std::string_view kTopSeparator =
    "*** *** *** *** *** CRASH DETECTED *** *** *** *** *** ***";
constexpr std::string_view kBottomSeparator =
    "*** *** *** *** *** END OF CRASH *** *** *** *** *** ***";

// Split "SIGSEGV / SEGV_MAPERR" into {"SIGSEGV", "SEGV_MAPERR"}.
// If there is no " / ", returns {signal_info, ""}.
std::pair<std::string, std::string> SplitSignalInfo(const std::string& signal_info) {
  constexpr std::string_view sep = " / ";
  const auto pos = signal_info.find(sep);
  if (pos == std::string::npos) {
    return {signal_info, ""};
  }
  return {signal_info.substr(0, pos), signal_info.substr(pos + sep.size())};
}

// Returns a non-empty string when the signal+address combination maps
// unambiguously to a known cause; empty string otherwise (omit the line).
// stack_ptr == 0 disables the stack-overflow check.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — three distinct uint64_t roles
std::string FormatProbableCause(uint32_t signal_number, uint64_t fault_addr, uint64_t prog_ctr,
                                uint64_t stack_ptr) {
  constexpr uint32_t sigsegv = 11;
  constexpr uint32_t sigabrt = 6;
  constexpr uint32_t sigbus = 7;
  constexpr uint32_t sigill = 4;
  constexpr uint32_t sigfpe = 8;
  constexpr uint64_t null_page_limit = 0x1000;
  constexpr uint64_t stack_proximity = 65536;

  if (signal_number == sigsegv) {
    if (fault_addr < null_page_limit) {
      if (fault_addr == 0) {
        return "null pointer dereference";
      }
      return absl::StrCat("null pointer dereference — access at offset 0x", absl::Hex(fault_addr));
    }
    if (fault_addr == prog_ctr) {
      return "execute fault — non-executable memory at pc "
             "(corrupt function pointer, smashed stack, or ROP)";
    }
    if (stack_ptr != 0) {
      const uint64_t delta =
          (fault_addr > stack_ptr) ? (fault_addr - stack_ptr) : (stack_ptr - fault_addr);
      if (delta < stack_proximity) {
        return absl::StrCat("stack overflow — fault address 0x", absl::Hex(fault_addr),
                            " is near stack pointer");
      }
    }
    return "";  // SIGSEGV but ambiguous — omit
  }
  if (signal_number == sigabrt) {
    return "abort() or assert() failure, or glibc heap corruption detected";
  }
  if (signal_number == sigbus) {
    return "misaligned memory access or truncated mmap file";
  }
  if (signal_number == sigill) {
    return "illegal instruction — corrupt code page or bad function pointer";
  }
  if (signal_number == sigfpe) {
    return "arithmetic exception — integer division by zero or overflow";
  }
  return "";
}

// Returns the stack pointer value from a register vector, or 0 if not found.
uint64_t ExtractStackPtr(const std::vector<std::pair<std::string, uint64_t>>& regs) {
  for (const auto& [name, val] : regs) {
    if (name == "sp" || name == "rsp") {
      return val;
    }
  }
  return 0;
}

}  // namespace

std::string FormatTombstone(const MinidumpInfo& info) {
  std::ostringstream out;

  out << kTopSeparator << "\n";

  // Process / thread header.
  const std::string& thread_name = (!info.threads.empty() && !info.threads[0].name.empty())
                                       ? info.threads[0].name
                                       : info.process_name;
  out << "process: " << info.process_name << " (pid " << info.pid << ")  "
      << "thread: " << thread_name << " (tid " << info.crashing_tid << ")\n";

  // Signal line.
  auto [sig_name, code_name] = SplitSignalInfo(info.signal_info);
  const auto hex_addr = std::format("0x{:016x}", info.fault_addr);
  if (code_name.empty()) {
    out << "signal " << info.signal_number << " (" << sig_name << "), fault addr " << hex_addr
        << "\n";
  } else {
    out << "signal " << info.signal_number << " (" << sig_name << "), code " << info.signal_code
        << " (" << code_name << "), fault addr " << hex_addr << "\n";
  }

  // Probable cause — emitted only when unambiguous.
  if (!info.threads.empty() && info.threads[0].is_crashing) {
    const uint64_t stack_ptr = ExtractStackPtr(info.threads[0].registers);
    const uint64_t prog_ctr = info.threads[0].frames.empty() ? 0 : info.threads[0].frames[0].pc;
    const std::string cause =
        FormatProbableCause(info.signal_number, info.fault_addr, prog_ctr, stack_ptr);
    if (!cause.empty()) {
      out << "probable cause: " << cause << "\n";
    }
  }

  if (!info.abort_message.empty()) {
    if (!info.terminate_type.empty()) {
      out << "Abort message: '" << info.terminate_type << ": " << info.abort_message << "'\n";
    } else {
      out << "Abort message: '" << info.abort_message << "'\n";
    }
  }

  out << "timestamp: " << info.timestamp << "\n";

  // Single crashing-thread PC line.
  if (!info.threads.empty() && info.threads[0].is_crashing && !info.threads[0].frames.empty()) {
    out << "\n";
    const auto& frame = info.threads[0].frames[0];
    if (frame.module_path.empty()) {
      out << std::format("pc 0x{:016x}  ???\n", frame.pc);
    } else if (!frame.build_id.empty()) {
      out << std::format("pc 0x{:016x}  {} (BuildId: {})\n", frame.module_offset, frame.module_path,
                         frame.build_id);
    } else {
      out << std::format("pc 0x{:016x}  {}\n", frame.module_offset, frame.module_path);
    }
  }

  out << "\nminidump saved to: " << info.minidump_path << "\n";
  out << kBottomSeparator << "\n";

  return out.str();
}

}  // namespace crashomon
