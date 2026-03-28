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

  // Process/thread line.
  char header[256];
  snprintf(header, sizeof(header),
           "pid: %" PRIu32 ", tid: %" PRIu32 ", name: %s  >>> %s <<<\n",
           info.pid, info.crashing_tid,
           info.process_name.c_str(), info.process_name.c_str());
  out << header;

  // Signal line.
  auto [sig_name, code_name] = SplitSignalInfo(info.signal_info);

  char sig_line[256];
  if (code_name.empty()) {
    snprintf(sig_line, sizeof(sig_line),
             "signal %" PRIu32 " (%s), fault addr 0x%016" PRIx64 "\n",
             info.signal_number, sig_name.c_str(), info.fault_addr);
  } else {
    snprintf(sig_line, sizeof(sig_line),
             "signal %" PRIu32 " (%s), code %" PRIu32 " (%s), "
             "fault addr 0x%016" PRIx64 "\n",
             info.signal_number, sig_name.c_str(),
             info.signal_code, code_name.c_str(),
             info.fault_addr);
  }
  out << sig_line;

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
    char thread_header[128];
    if (ti.name.empty()) {
      snprintf(thread_header, sizeof(thread_header),
               "\n--- --- --- thread %" PRIu32 " --- --- ---\n", ti.tid);
    } else {
      snprintf(thread_header, sizeof(thread_header),
               "\n--- --- --- thread %" PRIu32 " (%s) --- --- ---\n",
               ti.tid, ti.name.c_str());
    }
    out << thread_header;
    AppendFrames(out, ti.frames);
  }

  out << "\nminidump saved to: " << info.minidump_path << "\n";
  out << kSeparator << "\n";

  return out.str();
}

}  // namespace crashomon
