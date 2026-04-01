// tools/analyze/minidump_analyzer.h — minidump_stackwalk wrapper
//
// Invokes the minidump_stackwalk binary as a subprocess and captures its
// symbolicated output. The symbol store layout is Breakpad-standard:
//   <store>/<module_name>/<build_id>/<module_name>.sym
//
// This header is internal to crashomon-analyze.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace crashomon {

// Invoke `minidump_stackwalk <minidump_file> [symbol_paths...]` and return
// the combined stdout. The binary is searched via PATH or may be an absolute
// path.
//
// symbol_paths may be empty (produces raw unsymbolicated output).
absl::StatusOr<std::string> RunMinidumpStackwalk(std::string_view stackwalk_binary,
                                                 std::string_view minidump_file,
                                                 const std::vector<std::string>& symbol_paths);

// Prepare a temporary symbol store containing a single .sym file, invoke
// minidump_stackwalk against it, then remove the temp store.
//
// sym_file_path   — path to the existing .sym file
// minidump_file   — path to the minidump to analyse
// stackwalk_binary — path or name of minidump_stackwalk
absl::StatusOr<std::string> RunWithSingleSymFile(std::string_view stackwalk_binary,
                                                 std::string_view sym_file_path,
                                                 std::string_view minidump_file);

}  // namespace crashomon
