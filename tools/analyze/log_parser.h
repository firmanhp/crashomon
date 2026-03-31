// tools/analyze/log_parser.h — tombstone text parser
//
// Parses the crashomon tombstone format (as produced by FormatTombstone) back
// into structured form, for use by crashomon-analyze --stdin mode.
//
// This header is internal to crashomon-analyze.
// absl::StatusOr is safe here — not a public/cross-library interface.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace crashomon {

struct ParsedFrame {
  int index = 0;
  uint64_t module_offset = 0;
  std::string module_path;  // empty string if frame is unmapped ("???")
  // Any trailing text after the module path (e.g. already-symbolicated info).
  std::string trailing;
};

struct ParsedThread {
  uint32_t tid = 0;
  std::string name;          // optional thread name from "--- thread TID (name) ---"
  bool is_crashing = false;  // true for the backtrace: section
  std::vector<ParsedFrame> frames;
};

struct ParsedTombstone {
  uint32_t pid = 0;
  uint32_t crashing_tid = 0;
  std::string process_name;
  std::string signal_info;  // e.g. "SIGSEGV / SEGV_MAPERR"
  uint32_t signal_number = 0;
  uint32_t signal_code = 0;
  uint64_t fault_addr = 0;
  std::string timestamp;
  std::vector<ParsedThread> threads;  // crashing thread first
  std::string minidump_path;
};

// Parse a crashomon tombstone string into structured form.
// Returns an error if the text contains no recognisable tombstone header or
// no thread frames.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
absl::StatusOr<ParsedTombstone> ParseTombstone(const std::string& text);

}  // namespace crashomon
