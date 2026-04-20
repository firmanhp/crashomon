// tombstone/minidump_reader.h — Breakpad minidump processor wrapper
//
// Reads a Breakpad minidump file and extracts crash info, thread stacks,
// register state, and loaded modules. No symbol resolution is performed;
// all frames are raw PC values with module name + offset.
//
// absl::StatusOr is safe to use here — this is not a public API.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"

namespace crashomon {

struct FrameInfo {
  uint64_t pc = 0;             // absolute program counter
  uint64_t module_offset = 0;  // offset within the containing module
  std::string module_path;     // full path to the module; empty if unmapped
  std::string build_id;        // Breakpad code identifier; copied from module at extraction time
};

struct ThreadInfo {
  uint32_t tid = 0;
  std::string name;  // thread name, may be empty
  bool is_crashing = false;

  // AMD64 general-purpose registers, in display order.
  // Populated only for the crashing thread; empty for all others.
  std::vector<std::pair<std::string, uint64_t>> registers;

  std::vector<FrameInfo> frames;
};

struct ModuleInfo {
  std::string path;  // full path to the ELF binary
  uint64_t base_address = 0;
  uint64_t size = 0;
  std::string build_id;  // Breakpad code identifier — lowercase hex ELF build ID
};

struct MinidumpInfo {
  uint32_t pid = 0;
  std::string process_name;  // basename of the main executable

  // Full crash reason from Breakpad processor, e.g. "SIGSEGV / SEGV_MAPERR".
  // For tombstone formatting, split on " / " to get signal and code names.
  std::string signal_info;

  uint32_t signal_number = 0;  // si_signo (from exception_code)
  uint32_t signal_code = 0;    // si_code  (from exception_flags)
  uint64_t fault_addr = 0;
  uint32_t crashing_tid = 0;
  std::string timestamp;  // ISO 8601 UTC (from minidump time_date_stamp)

  // Crashing thread is always first in the list (if present).
  std::vector<ThreadInfo> threads;
  std::vector<ModuleInfo> modules;

  std::string minidump_path;
  std::string abort_message;  // Crashpad "abort_message" annotation; empty if absent
  std::string terminate_type; // Crashpad "terminate_type" annotation; empty if absent
};

// Parse a Breakpad minidump file and populate MinidumpInfo.
// Uses Breakpad's MinidumpProcessor with no symbol supplier (raw addresses).
// Returns an error status if the file cannot be read or processed.
absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path);

}  // namespace crashomon
