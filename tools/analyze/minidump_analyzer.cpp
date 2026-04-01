// tools/analyze/minidump_analyzer.cpp — minidump_stackwalk wrapper

#include "tools/analyze/minidump_analyzer.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace crashomon {
namespace {

// Shell-quote a single token.
std::string ShellQuote(std::string_view str) {
  std::string out = "'";
  for (const char cur : str) {
    if (cur == '\'') {
      out += "'\\''";
    } else {
      out += cur;
    }
  }
  out += "'";
  return out;
}

// Capture stdout from `cmd`.
absl::StatusOr<std::string> CaptureCommand(const std::string& cmd) {
  // popen is intentional: this tool's purpose is to invoke
  // minidump_stackwalk as a subprocess and capture its stdout.
  // NOLINTNEXTLINE(cert-env33-c, misc-include-cleaner)
  FILE* pipe = popen(cmd.c_str(), "r");  // popen/pclose via <cstdio>; include-cleaner FP.
  if (pipe == nullptr) {
    return absl::InternalError(std::string("popen failed for: ") + cmd);
  }
  constexpr size_t read_buf_size = 4096;
  std::string out;
  std::array<char, read_buf_size> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
  }
  const int ret_code = pclose(pipe);  // NOLINT(misc-include-cleaner) — pclose comes via <cstdio>
                                      // which is included; false positive from include-cleaner.
  if (ret_code != 0 && out.empty()) {
    return absl::InternalError(std::string("minidump_stackwalk exited with code ") +
                               std::to_string(ret_code));
  }
  return out;
}

// Parse MODULE line from a Breakpad .sym file.
// Format: "MODULE Linux x86_64 <build_id> <module_name>"
// Returns {module_name, build_id} or an error.
absl::StatusOr<std::pair<std::string, std::string>> ParseSymModuleLine(
    const std::string& sym_path) {
  std::ifstream file(sym_path);
  if (!file) {
    return absl::NotFoundError(std::string("Cannot open sym file: ") + sym_path);
  }
  std::string first_line;
  if (!std::getline(file, first_line)) {
    return absl::InvalidArgumentError(std::string("Empty sym file: ") + sym_path);
  }
  // Expected: "MODULE <os> <arch> <build_id> <module_name>" — arch is parsed but ignored.
  std::istringstream stream(first_line);
  std::string token;
  std::string os_name;
  std::string arch;
  std::string build_id;
  std::string module_name;
  if (!(stream >> token >> os_name >> arch >> build_id >> module_name) || token != "MODULE") {
    return absl::InvalidArgumentError(std::string("Bad MODULE line in: ") + sym_path +
                                      "\n  got: " + first_line);
  }
  return std::make_pair(
      module_name, build_id);  // NOLINT(misc-include-cleaner) — std::make_pair comes via <utility>
                               // which is included; false positive from include-cleaner.
}

}  // namespace

absl::StatusOr<std::string> RunMinidumpStackwalk(
    // stackwalk_binary (path to tool) and
    // minidump_file (input file) are semantically distinct roles; names are unambiguous.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::string_view stackwalk_binary, std::string_view minidump_file,
    const std::vector<std::string>& symbol_paths) {
  // minidump_stackwalk [-m] [-s] <minidump-file> [<symbol-path> ...]
  std::string cmd = ShellQuote(stackwalk_binary);
  cmd += " " + ShellQuote(minidump_file);
  for (const auto& sym_path : symbol_paths) {
    cmd += " " + ShellQuote(sym_path);
  }
  cmd += " 2>/dev/null";
  return CaptureCommand(cmd);
}

absl::StatusOr<std::string> RunWithSingleSymFile(
    // stackwalk_binary (path to tool),
    // sym_file_path (.sym input), and minidump_file (crash input) have distinct roles; names are
    // unambiguous.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    std::string_view stackwalk_binary, std::string_view sym_file_path,
    std::string_view minidump_file) {
  auto module_or = ParseSymModuleLine(std::string(sym_file_path));
  if (!module_or.ok()) {
    return module_or.status();
  }
  const auto& [module_name, build_id] = *module_or;

  // Build a temporary Breakpad store layout:
  //   <tmpdir>/<module_name>/<build_id>/<module_name>.sym
  auto tmp_base = std::filesystem::temp_directory_path() / "crashomon_analyze_sym_XXXXXX";
  std::string tmp_template = tmp_base.string();
  // mkdtemp comes via <cstdlib> which is transitively included; include-cleaner FP.
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (mkdtemp(tmp_template.data()) == nullptr) {
    return absl::InternalError("mkdtemp failed");
  }

  const std::filesystem::path sym_dir =
      std::filesystem::path(tmp_template) / module_name / build_id;
  // std::error_code comes via <system_error> which is included; include-cleaner FP.
  // NOLINTNEXTLINE(misc-include-cleaner)
  std::error_code err;
  std::filesystem::create_directories(sym_dir, err);
  if (err) {
    std::filesystem::remove_all(tmp_template);
    return absl::InternalError(std::string("create_directories: ") + err.message());
  }

  // Hard-link or copy the .sym file into place.
  const std::filesystem::path dest = sym_dir / (module_name + ".sym");
  std::filesystem::copy_file(sym_file_path, dest, err);
  if (err) {
    std::filesystem::remove_all(tmp_template);
    return absl::InternalError(std::string("copy_file: ") + err.message());
  }

  auto result = RunMinidumpStackwalk(stackwalk_binary, minidump_file, {tmp_template});
  std::filesystem::remove_all(tmp_template);
  return result;
}

}  // namespace crashomon
