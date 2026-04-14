// tombstone/tombstone_formatter.cpp — Android-style tombstone formatter

#include "tombstone/tombstone_formatter.h"

#include <cstddef>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tombstone/minidump_reader.h"

namespace crashomon {
namespace {

constexpr std::string_view kSeparator =
    "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***";

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

// build_ids maps module_path → build_id hex string (from MinidumpInfo::modules).
void AppendFrames(std::ostringstream& out, const std::vector<FrameInfo>& frames,
                  const std::unordered_map<std::string_view, std::string_view>& build_ids) {
  for (size_t idx = 0; idx < frames.size(); ++idx) {
    const auto& frame = frames[idx];
    if (frame.module_path.empty()) {
      out << std::format("    #{} pc 0x{:016x}  ???\n", idx, frame.pc);
    } else {
      const auto found = build_ids.find(frame.module_path);
      if (found != build_ids.end() && !found->second.empty()) {
        out << std::format("    #{} pc 0x{:016x}  {} (BuildId: {})\n", idx, frame.module_offset,
                           frame.module_path, found->second);
      } else {
        out << std::format("    #{} pc 0x{:016x}  {}\n", idx, frame.module_offset,
                           frame.module_path);
      }
    }
  }
}

}  // namespace

std::string FormatTombstone(const MinidumpInfo& info) {
  std::ostringstream out;

  // Build path → build_id lookup from module list (populated by CollectModules).
  std::unordered_map<std::string_view, std::string_view> build_ids;
  build_ids.reserve(info.modules.size());
  for (const auto& mod : info.modules) {
    if (!mod.build_id.empty()) {
      build_ids.emplace(mod.path, mod.build_id);
    }
  }

  out << kSeparator << "\n";

  // Process/thread line — written directly to avoid fixed-size buffer truncation.
  out << "pid: " << info.pid << ", tid: " << info.crashing_tid << ", name: " << info.process_name
      << "  >>> " << info.process_name << " <<<\n";

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

  out << "timestamp: " << info.timestamp << "\n";

  // Crashing thread registers (first thread in the list, if crashing).
  if (!info.threads.empty() && info.threads[0].is_crashing && !info.threads[0].registers.empty()) {
    out << "\n";
    const auto& regs = info.threads[0].registers;
    for (size_t reg_idx = 0; reg_idx < regs.size();) {
      out << "   ";
      // Up to 3 registers per line.
      for (size_t col = 0; col < 3 && reg_idx < regs.size(); ++col, ++reg_idx) {
        out << std::format(" {:<3s} {:016x}", regs[reg_idx].first, regs[reg_idx].second);
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
    AppendFrames(out, info.threads[0].frames, build_ids);
  }

  out << "\nminidump saved to: " << info.minidump_path << "\n";
  out << kSeparator << "\n";

  return out.str();
}

}  // namespace crashomon
