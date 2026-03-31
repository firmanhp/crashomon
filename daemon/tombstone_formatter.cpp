// daemon/tombstone_formatter.cpp — Android-style tombstone formatter

#include "daemon/tombstone_formatter.h"

#include <cinttypes>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace crashomon {
namespace {

constexpr std::string_view kSeparator =
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***";

// Split "SIGSEGV / SEGV_MAPERR" into {"SIGSEGV", "SEGV_MAPERR"}.
// If there is no " / ", returns {signal_info, ""}.
std::pair<std::string, std::string> SplitSignalInfo(
    const std::string& signal_info) {
  constexpr std::string_view sep = " / ";
  const auto pos = signal_info.find(sep);
  if (pos == std::string::npos) {
    return {signal_info, ""};
  }
  return {signal_info.substr(0, pos),
          signal_info.substr(pos + sep.size())};
}

void AppendFrames(std::ostringstream& out,
                  const std::vector<FrameInfo>& frames) {
  for (size_t idx = 0; idx < frames.size(); ++idx) {
    const auto& frame = frames[idx];
    if (frame.module_path.empty()) {
      out << "    #" << idx << " pc 0x";
      std::array<char, 20> hex{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c)
      (void)std::snprintf(hex.data(), hex.size(), "%016" PRIx64, frame.pc);
      out << hex.data() << "  ???\n";
    } else {
      out << "    #" << idx << " pc 0x";
      std::array<char, 20> hex{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c)
      (void)std::snprintf(hex.data(), hex.size(), "%016" PRIx64, frame.module_offset);
      out << hex.data() << "  " << frame.module_path << "\n";
    }
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

  std::array<char, 32> hex_addr{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c)
  (void)std::snprintf(hex_addr.data(), hex_addr.size(), "0x%016" PRIx64, info.fault_addr);
  if (code_name.empty()) {
    out << "signal " << info.signal_number
        << " (" << sig_name << "), fault addr " << hex_addr.data() << "\n";
  } else {
    out << "signal " << info.signal_number
        << " (" << sig_name << "), code " << info.signal_code
        << " (" << code_name << "), fault addr " << hex_addr.data() << "\n";
  }

  out << "timestamp: " << info.timestamp << "\n";

  // Crashing thread registers (first thread in the list, if crashing).
  if (!info.threads.empty() && info.threads[0].is_crashing &&
      !info.threads[0].registers.empty()) {
    out << "\n";
    const auto& regs = info.threads[0].registers;
    for (size_t reg_idx = 0; reg_idx < regs.size(); ) {
      out << "   ";
      // Up to 3 registers per line.
      for (size_t col = 0; col < 3 && reg_idx < regs.size(); ++col, ++reg_idx) {
        std::array<char, 32> reg_buf{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c)
        (void)std::snprintf(reg_buf.data(), reg_buf.size(), " %-3s %016" PRIx64,
                            regs[reg_idx].first.c_str(), regs[reg_idx].second);
        out << reg_buf.data();
        if (col < 2 && (reg_idx + 1) < regs.size()) {
          out << " ";
        }
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
  for (size_t thr_idx = 1; thr_idx < info.threads.size(); ++thr_idx) {
    const auto& thread = info.threads[thr_idx];
    if (thread.name.empty()) {
      out << "\n--- --- --- thread " << thread.tid << " --- --- ---\n";
    } else {
      out << "\n--- --- --- thread " << thread.tid << " (" << thread.name << ") --- --- ---\n";
    }
    AppendFrames(out, thread.frames);
  }

  out << "\nminidump saved to: " << info.minidump_path << "\n";
  out << kSeparator << "\n";

  return out.str();
}

}  // namespace crashomon
