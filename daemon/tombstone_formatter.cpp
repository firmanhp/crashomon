// daemon/tombstone_formatter.cpp — Android-style tombstone formatter

#include "daemon/tombstone_formatter.h"

#include <cinttypes>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace crashomon {
namespace {

static constexpr const char kSeparator[] =
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***";

// Split "SIGSEGV / SEGV_MAPERR" into {"SIGSEGV", "SEGV_MAPERR"}.
// If there is no " / ", returns {signal_info, ""}.
std::pair<std::string, std::string> SplitSignalInfo(
    const std::string& signal_info) {
  static constexpr const char kSep[] = " / ";
  auto pos = signal_info.find(kSep);
  if (pos == std::string::npos) return {signal_info, ""};
  return {signal_info.substr(0, pos),
          signal_info.substr(pos + sizeof(kSep) - 1)};
}

void AppendFrames(std::ostringstream& out,
                  const std::vector<FrameInfo>& frames) {
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& f = frames[i];
    char line[256];
    if (f.module_path.empty()) {
      snprintf(line, sizeof(line), "    #%02zu pc 0x%016" PRIx64 "  ???\n",
               i, f.pc);
    } else {
      snprintf(line, sizeof(line),
               "    #%02zu pc 0x%016" PRIx64 "  %s\n",
               i, f.module_offset, f.module_path.c_str());
    }
    out << line;
  }
}

}  // namespace

std::string FormatTombstone(const MinidumpInfo& info) {
  std::ostringstream out;

  out << kSeparator << "\n";

  // Process/thread line — written directly to avoid fixed-size buffer truncation.
  out << "pid: " << info.pid
      << ", tid: " << info.crashing_tid
      << ", name: " << info.process_name
      << "  >>> " << info.process_name << " <<<\n";

  // Signal line.
  auto [sig_name, code_name] = SplitSignalInfo(info.signal_info);

  char hex_addr[32];
  snprintf(hex_addr, sizeof(hex_addr), "0x%016" PRIx64, info.fault_addr);
  if (code_name.empty()) {
    out << "signal " << info.signal_number
        << " (" << sig_name << "), fault addr " << hex_addr << "\n";
  } else {
    out << "signal " << info.signal_number
        << " (" << sig_name << "), code " << info.signal_code
        << " (" << code_name << "), fault addr " << hex_addr << "\n";
  }

  out << "timestamp: " << info.timestamp << "\n";

  // Crashing thread registers (first thread in the list, if crashing).
  if (!info.threads.empty() && info.threads[0].is_crashing &&
      !info.threads[0].registers.empty()) {
    out << "\n";
    const auto& regs = info.threads[0].registers;
    for (size_t i = 0; i < regs.size(); ) {
      out << "   ";
      // Up to 3 registers per line.
      for (size_t col = 0; col < 3 && i < regs.size(); ++col, ++i) {
        char reg[32];
        snprintf(reg, sizeof(reg), " %-3s %016" PRIx64,
                 regs[i].first.c_str(), regs[i].second);
        out << reg;
        if (col < 2 && (i + 1) < regs.size()) out << " ";
      }
      out << "\n";
    }
  }

  // Crashing thread backtrace.
  if (!info.threads.empty() && info.threads[0].is_crashing) {
    out << "\nbacktrace:\n";
    AppendFrames(out, info.threads[0].frames);
  }

  // Other threads.
  for (size_t t = 1; t < info.threads.size(); ++t) {
    const auto& ti = info.threads[t];
    if (ti.name.empty()) {
      out << "\n--- --- --- thread " << ti.tid << " --- --- ---\n";
    } else {
      out << "\n--- --- --- thread " << ti.tid << " (" << ti.name << ") --- --- ---\n";
    }
    AppendFrames(out, ti.frames);
  }

  out << "\nminidump saved to: " << info.minidump_path << "\n";
  out << kSeparator << "\n";

  return out.str();
}

}  // namespace crashomon
