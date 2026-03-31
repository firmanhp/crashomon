// tools/analyze/minidump_analyzer.cpp — minidump_stackwalk wrapper

#include "tools/analyze/minidump_analyzer.h"

#include <sys/stat.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"

namespace crashomon {
namespace {

// Shell-quote a single token.
std::string ShellQuote(std::string_view str) {
  std::string out = "'";
  for (const char cur : str) {
    if (cur == '\'') { out += "'\\''"; } else { out += cur; }
  }
  out += "'";
  return out;
}

// Capture stdout from `cmd`.
absl::StatusOr<std::string> CaptureCommand(const std::string& cmd) {
  FILE* pipe = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c)
  if (pipe == nullptr) {
    return absl::InternalError(std::string("popen failed for: ") + cmd);
  }
  std::string out;
  std::array<char, 4096> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
  }
  const int ret_code = pclose(pipe);
  if (ret_code != 0 && out.empty()) {
    return absl::InternalError(
        std::string("minidump_stackwalk exited with code ") +
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
    return absl::NotFoundError(
        std::string("Cannot open sym file: ") + sym_path);
  }
  std::string first_line;
  if (!std::getline(file, first_line)) {
    return absl::InvalidArgumentError(
        std::string("Empty sym file: ") + sym_path);
  }
  // Expected: "MODULE <os> <arch> <build_id> <module_name>" — arch is parsed but ignored.
  std::istringstream stream(first_line);
  std::string token;
  std::string os_name;
  std::string arch;
  std::string build_id;
  std::string module_name;
  if (!(stream >> token >> os_name >> arch >> build_id >> module_name) ||
      token != "MODULE") {
    return absl::InvalidArgumentError(
        std::string("Bad MODULE line in: ") + sym_path +
        "\n  got: " + first_line);
  }
  return std::make_pair(module_name, build_id);
}

}  // namespace

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
absl::StatusOr<std::string> RunMinidumpStackwalk(
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
    std::string_view stackwalk_binary, std::string_view sym_file_path,
    std::string_view minidump_file) {
  auto module_or = ParseSymModuleLine(std::string(sym_file_path));
  if (!module_or.ok()) { return module_or.status(); }
  const auto& [module_name, build_id] = *module_or;

  // Build a temporary Breakpad store layout:
  //   <tmpdir>/<module_name>/<build_id>/<module_name>.sym
  auto tmp_base = std::filesystem::temp_directory_path() /
                  "crashomon_analyze_sym_XXXXXX";
  std::string tmp_template = tmp_base.string();
  if (mkdtemp(tmp_template.data()) == nullptr) {
    return absl::InternalError("mkdtemp failed");
  }

  const std::filesystem::path sym_dir =
      std::filesystem::path(tmp_template) / module_name / build_id;
  std::error_code err;
  std::filesystem::create_directories(sym_dir, err);
  if (err) {
    std::filesystem::remove_all(tmp_template);
    return absl::InternalError(
        std::string("create_directories: ") + err.message());
  }

  // Hard-link or copy the .sym file into place.
  const std::filesystem::path dest = sym_dir / (module_name + ".sym");
  std::filesystem::copy_file(sym_file_path, dest, err);
  if (err) {
    std::filesystem::remove_all(tmp_template);
    return absl::InternalError(std::string("copy_file: ") + err.message());
  }

  auto result = RunMinidumpStackwalk(stackwalk_binary, minidump_file,
                                     {tmp_template});
  std::filesystem::remove_all(tmp_template);
  return result;
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace crashomon
