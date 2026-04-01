// tools/analyze/log_parser.cpp — tombstone text parser

#include "tools/analyze/log_parser.h"

#include <cstddef>
#include <cstdint>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace crashomon {
namespace {

// Frame: "    #00 pc 0x00001a0  /usr/bin/my_service (symbol) [src:line]"
// Groups: 1=index  2=hex_offset  3=module_path_or_???  4=trailing(optional)
const std::regex kFrameRe(R"(^\s{2,}#(\d+)\s+pc\s+(0x[0-9a-fA-F]+)\s+(\S+)(?:\s+(.+))?$)");

// "pid: 1234, tid: 1234, name: my_service  >>> my_service <<<"
const std::regex kHeaderRe(R"(pid:\s*(\d+),\s*tid:\s*(\d+),\s*name:\s*(\S+))");

// "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0xdeadbeef"
// code group is optional (e.g. SIGABRT has no code in our format)
const std::regex kSignalRe(
    R"(signal\s+(\d+)\s+\(([^)]+)\)(?:,\s*code\s+(\d+)\s+\(([^)]+)\))?[^0-9a-fx]*fault addr\s+(0x[0-9a-fA-F]+))");

// "timestamp: 2026-03-27T10:15:30Z"
const std::regex kTimestampRe(R"(timestamp:\s*(\S+))");

// "--- --- --- thread 1235 --- --- ---"
// or "--- --- --- thread 1235 (worker) --- --- ---"
const std::regex kThreadRe(
    R"(---\s+---\s+---\s+thread\s+(\d+)(?:\s+\(([^)]+)\))?\s+---\s+---\s+---)");

// "minidump saved to: /var/crashomon/xxx.dmp"
const std::regex kMinidumpPathRe(R"(minidump saved to:\s*(\S+))");

// ── Per-line parse helpers ────────────────────────────────────────────────────

struct ParseState {
  ParsedTombstone result;
  ParsedThread working;
  bool in_backtrace = false;
  bool in_other_thread = false;
};

// Returns true if the line was consumed.
bool TryHeader(const std::string& line, std::smatch& match, ParseState& state) {
  if (!std::regex_search(line, match, kHeaderRe)) {
    return false;
  }
  state.result.pid = static_cast<uint32_t>(std::stoul(match[1].str()));
  state.result.crashing_tid = static_cast<uint32_t>(std::stoul(match[2].str()));
  state.result.process_name = match[3].str();
  state.working.tid = state.result.crashing_tid;
  return true;
}

bool TrySignal(const std::string& line, std::smatch& match, ParseState& state) {
  if (!std::regex_search(line, match, kSignalRe)) {
    return false;
  }
  state.result.signal_number = static_cast<uint32_t>(std::stoul(match[1].str()));
  state.result.signal_info = match[2].str();
  if (match[3].matched) {
    state.result.signal_code = static_cast<uint32_t>(std::stoul(match[3].str()));
    state.result.signal_info += " / " + match[4].str();
  }
  // kFaultAddrGroup: "signal N (sig), code N (code), fault addr 0x..." → group 5 = fault addr.
  constexpr int base16 = 16;
  constexpr size_t fault_addr_group = 5;  // NOLINT(misc-include-cleaner) — size_t comes via
                                          // <cstddef> transitively; included via log_parser.h.
  state.result.fault_addr =
      static_cast<uint64_t>(std::stoull(match[fault_addr_group].str(), nullptr, base16));
  return true;
}

bool TryTimestamp(const std::string& line, std::smatch& match, ParseState& state) {
  if (!std::regex_search(line, match, kTimestampRe)) {
    return false;
  }
  state.result.timestamp = match[1].str();
  return true;
}

bool TryThreadSep(const std::string& line, std::smatch& match, ParseState& state) {
  if (!std::regex_search(line, match, kThreadRe)) {
    return false;
  }
  if (state.in_backtrace || state.in_other_thread) {
    state.result.threads.push_back(std::move(state.working));
  }
  state.working = ParsedThread();
  state.working.tid = static_cast<uint32_t>(std::stoul(match[1].str()));
  state.working.is_crashing = false;
  if (match[2].matched) {
    state.working.name = match[2].str();
  }
  state.in_other_thread = true;
  state.in_backtrace = false;
  return true;
}

bool TryMinidumpPath(const std::string& line, std::smatch& match, ParseState& state) {
  if (!std::regex_search(line, match, kMinidumpPathRe)) {
    return false;
  }
  state.result.minidump_path = match[1].str();
  if (state.in_backtrace || state.in_other_thread) {
    state.result.threads.push_back(std::move(state.working));
    state.working = ParsedThread();
    state.in_backtrace = false;
    state.in_other_thread = false;
  }
  return true;
}

bool TryFrame(const std::string& line, std::smatch& match, ParseState& state) {
  if (!(state.in_backtrace || state.in_other_thread)) {
    return false;
  }
  if (!std::regex_search(line, match, kFrameRe)) {
    return false;
  }
  ParsedFrame frame;
  frame.index = std::stoi(match[1].str());
  constexpr int base16 = 16;
  frame.module_offset = static_cast<uint64_t>(std::stoull(match[2].str(), nullptr, base16));
  const std::string mod_str = match[3].str();
  frame.module_path = (mod_str == "???") ? "" : mod_str;
  if (match[4].matched) {
    frame.trailing = match[4].str();
  }
  state.working.frames.push_back(std::move(frame));
  return true;
}

}  // namespace

absl::StatusOr<ParsedTombstone> ParseTombstone(const std::string& text) {
  if (text.empty()) {
    return absl::InvalidArgumentError("Empty input");
  }

  ParseState state;
  state.working.is_crashing = true;

  std::smatch match;
  std::istringstream stream(text);
  std::string line;

  while (std::getline(stream, line)) {
    if (TryHeader(line, match, state)) {
      continue;
    }
    if (TrySignal(line, match, state)) {
      continue;
    }
    if (TryTimestamp(line, match, state)) {
      continue;
    }
    if (line.find("backtrace:") != std::string::npos) {
      state.in_backtrace = true;
      state.in_other_thread = false;
      continue;
    }
    if (TryThreadSep(line, match, state)) {
      continue;
    }
    if (TryMinidumpPath(line, match, state)) {
      continue;
    }
    (void)TryFrame(line, match, state);
  }

  // Flush any final thread not yet saved (e.g. no minidump path line).
  if (!state.working.frames.empty() || (state.working.tid != 0 && state.in_other_thread)) {
    state.result.threads.push_back(std::move(state.working));
  }

  if (state.result.threads.empty()) {
    return absl::InvalidArgumentError("No thread frames found — not a valid tombstone");
  }

  return state.result;
}

}  // namespace crashomon
