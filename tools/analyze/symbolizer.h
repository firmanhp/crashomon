// tools/analyze/symbolizer.h — eu-addr2line-based frame symbolizer
//
// Symbolicates ParsedTombstone frames using eu-addr2line.
// For each unique module in the tombstone, invokes eu-addr2line once with all
// offsets for that module, then maps results back to frame positions.
//
// This header is internal to crashomon-analyze.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"
#include "tools/analyze/log_parser.h"

namespace crashomon {

struct SymbolInfo {
  std::string function;    // function name; "??" if unknown
  std::string source_file; // source filename (no directory); empty if unknown
  int source_line = 0;     // source line; 0 if unknown
};

// Key: {tid, frame_index}
using SymbolTable = std::map<std::pair<uint32_t, int>, SymbolInfo>;

// Symbolize all frames in `tombstone` using `addr2line_binary` (typically
// "eu-addr2line" or "addr2line").
// Frames with no module path (unmapped) are silently skipped.
// Frames whose module binary cannot be found on disk are skipped with a
// warning in the returned table (SymbolInfo with function="??").
absl::StatusOr<SymbolTable> SymbolizeWithAddrLine(
    const ParsedTombstone& tombstone, std::string_view addr2line_binary);

// Format a tombstone with inline symbol information.
// Replaces unsymbolicated frame lines with lines that include the function
// name and source location where available.
std::string FormatSymbolicated(const ParsedTombstone& tombstone,
                                const SymbolTable& symbols);

}  // namespace crashomon
