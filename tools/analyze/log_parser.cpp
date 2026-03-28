// tools/analyze/log_parser.cpp — tombstone text parser

#include "tools/analyze/log_parser.h"

#include <regex>
#include <sstream>
#include <string>

namespace crashomon {
namespace {

// Frame: "    #00 pc 0x00001a0  /usr/bin/my_service (symbol) [src:line]"
// Groups: 1=index  2=hex_offset  3=module_path_or_???  4=trailing(optional)
const std::regex kFrameRe(
    R"(^\s{2,}#(\d+)\s+pc\s+(0x[0-9a-fA-F]+)\s+(\S+)(?:\s+(.+))?$)");

// "pid: 1234, tid: 1234, name: my_service  >>> my_service <<<"
const std::regex kHeaderRe(
    R"(pid:\s*(\d+),\s*tid:\s*(\d+),\s*name:\s*(\S+))");

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

}  // namespace

absl::StatusOr<ParsedTombstone> ParseTombstone(const std::string& text) {
  if (text.empty()) {
    return absl::InvalidArgumentError("Empty input");
  }

  ParsedTombstone result;

  // Working thread accumulates frames until we hit a new thread or end.
  ParsedThread working;
  working.is_crashing = true;
  bool in_backtrace = false;
  bool in_other_thread = false;

  std::smatch m;
  std::istringstream ss(text);
  std::string line;

  while (std::getline(ss, line)) {
    // Header
    if (std::regex_search(line, m, kHeaderRe)) {
      result.pid = static_cast<uint32_t>(std::stoul(m[1].str()));
      result.crashing_tid = static_cast<uint32_t>(std::stoul(m[2].str()));
      result.process_name = m[3].str();
      working.tid = result.crashing_tid;
      continue;
    }
    // Signal
    if (std::regex_search(line, m, kSignalRe)) {
      result.signal_number = static_cast<uint32_t>(std::stoul(m[1].str()));
      result.signal_info = m[2].str();
      if (m[3].matched) {
        result.signal_code = static_cast<uint32_t>(std::stoul(m[3].str()));
        result.signal_info += " / " + m[4].str();
      }
      result.fault_addr =
          static_cast<uint64_t>(std::stoull(m[5].str(), nullptr, 16));
      continue;
    }
    // Timestamp
    if (std::regex_search(line, m, kTimestampRe)) {
      result.timestamp = m[1].str();
      continue;
    }
    // "backtrace:" section marker — frames that follow belong to crashing thread
    if (line.find("backtrace:") != std::string::npos) {
      in_backtrace = true;
      in_other_thread = false;
      continue;
    }
    // Thread separator — save current thread and start a new one
    if (std::regex_search(line, m, kThreadRe)) {
      if (in_backtrace || in_other_thread) {
        result.threads.push_back(std::move(working));
      }
      working = ParsedThread();
      working.tid = static_cast<uint32_t>(std::stoul(m[1].str()));
      working.is_crashing = false;
      if (m[2].matched) working.name = m[2].str();
      in_other_thread = true;
      in_backtrace = false;
      continue;
    }
    // Minidump path
    if (std::regex_search(line, m, kMinidumpPathRe)) {
      result.minidump_path = m[1].str();
      if (in_backtrace || in_other_thread) {
        result.threads.push_back(std::move(working));
        working = ParsedThread();
        in_backtrace = false;
        in_other_thread = false;
      }
      continue;
    }
    // Frame line
    if ((in_backtrace || in_other_thread) &&
        std::regex_search(line, m, kFrameRe)) {
      ParsedFrame f;
      f.index = std::stoi(m[1].str());
      f.module_offset =
          static_cast<uint64_t>(std::stoull(m[2].str(), nullptr, 16));
      std::string mod = m[3].str();
      f.module_path = (mod == "???") ? "" : mod;
      if (m[4].matched) f.trailing = m[4].str();
      working.frames.push_back(std::move(f));
    }
  }

  // Flush any final thread not yet saved (e.g. no minidump path line).
  if (!working.frames.empty() || (working.tid != 0 && in_other_thread)) {
    result.threads.push_back(std::move(working));
  }

  if (result.threads.empty()) {
    return absl::InvalidArgumentError(
        "No thread frames found — not a valid tombstone");
  }

  return result;
}

}  // namespace crashomon
