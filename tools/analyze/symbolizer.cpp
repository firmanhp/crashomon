// tools/analyze/symbolizer.cpp — eu-addr2line-based frame symbolizer

#include "tools/analyze/symbolizer.h"

#include <sys/stat.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "tools/analyze/log_parser.h"

namespace crashomon {
namespace {

struct FrameRef {
  uint32_t tid;
  int index;
  uint64_t offset;
};

// Run `cmd` and return its stdout. Returns empty string on error.
// The command is built by the caller; no shell metacharacters should appear
// in untrusted values (module paths come from tombstone text).
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::string RunCapture(const std::string& cmd) {
  // popen is intentional: this tool's purpose is to invoke addr2line
  // as a subprocess and capture its stdout.
  // NOLINTNEXTLINE(cert-env33-c, misc-include-cleaner)
  FILE* pipe = popen(cmd.c_str(), "r");  // popen/pclose via <cstdio>; include-cleaner FP.
  if (pipe == nullptr) {
    return "";
  }
  constexpr size_t read_buf_size = 256;
  std::string out;
  std::array<char, read_buf_size> buf{};
  while (fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
    out += buf.data();
  }
  const int exit_code = pclose(pipe);  // NOLINT(misc-include-cleaner) — pclose comes via <cstdio>
                                       // which is included; false positive from include-cleaner.
  if (exit_code != 0) {
    return "";
  }
  return out;
}

// Escape a path so it is safe to embed in a shell command.
// We only allow paths that contain no shell-special characters;
// anything else is wrapped in single quotes with embedded ' escaped.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::string ShellQuote(const std::string& str) {
  std::string out = "'";
  for (const char chr : str) {
    if (chr == '\'') {
      out += "'\\''";
    } else {
      out += chr;
    }
  }
  out += "'";
  return out;
}

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
bool FileExists(const std::string& path) {
  struct stat stat_buf {};
  return ::stat(path.c_str(), &stat_buf) == 0 && S_ISREG(stat_buf.st_mode);
}

// Call addr2line once for all offsets belonging to one module.
// Returns lines in order: 2 lines per address (function, then file:line).
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::vector<std::string> QueryAddrLine(const std::string& binary, const std::string& addr2line,
                                       const std::vector<FrameRef>& refs) {
  if (refs.empty()) {
    return {};
  }

  // Build: addr2line -e <module> -f -s -p <offset1> <offset2> ...
  // -f: print function name  -s: strip directory  -p: pretty-print (one line)
  // We use plain -f (two lines per addr) for simpler parsing.
  std::string cmd = ShellQuote(addr2line) + " -e " + ShellQuote(binary) + " -f -s";
  for (const auto& ref : refs) {
    cmd += std::format(" 0x{:016x}", ref.offset);
  }
  cmd += " 2>/dev/null";

  const std::string raw = RunCapture(cmd);
  std::vector<std::string> lines;
  std::istringstream stream(raw);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

// func_line/loc_line are distinct addr2line output fields; conventional return type notation is
// clearer per Google Style Guide.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters,modernize-use-trailing-return-type)
SymbolInfo ParseAddrLineOutput(const std::string& func_line, const std::string& loc_line) {
  SymbolInfo sym;
  sym.function = func_line;
  if (sym.function.empty()) {
    sym.function = "??";
  }

  // loc_line is "file.c:142" or "??:0"
  const auto colon = loc_line.rfind(':');
  if (colon != std::string::npos) {
    sym.source_file = loc_line.substr(0, colon);
    const std::string num_str = loc_line.substr(colon + 1);
    char* end = nullptr;
    const long val =
        std::strtol(num_str.c_str(), &end,
                    10);  // NOLINT(misc-include-cleaner) — std::strtol comes via <cstdlib> which is
                          // included; false positive from include-cleaner.
    sym.source_line = (end != num_str.c_str()) ? static_cast<int>(val) : 0;
  }
  if (sym.source_file == "??") {
    sym.source_file.clear();
  }
  return sym;
}

}  // namespace

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
absl::StatusOr<SymbolTable> SymbolizeWithAddrLine(const ParsedTombstone& tombstone,
                                                  std::string_view addr2line_binary) {
  // Group frames by module path.
  std::map<std::string, std::vector<FrameRef>> by_module;
  for (const auto& thread : tombstone.threads) {
    for (const auto& frame : thread.frames) {
      if (frame.module_path.empty()) {
        continue;
      }
      by_module[frame.module_path].push_back(
          {.tid = thread.tid, .index = frame.index, .offset = frame.module_offset});
    }
  }

  SymbolTable result;
  const std::string binary(addr2line_binary);

  for (const auto& [module, refs] : by_module) {
    if (!FileExists(module)) {
      // Binary not found on this machine — mark frames as unknown.
      for (const auto& ref : refs) {
        result[{ref.tid, ref.index}] = {"??", "", 0};
      }
      continue;
    }

    const std::vector<std::string> lines = QueryAddrLine(module, binary, refs);

    // Expect 2 lines per address.
    for (size_t i = 0; i < refs.size(); ++i) {
      const size_t base = i * 2;
      const std::string func = (base < lines.size()) ? lines[base] : "??";
      const std::string loc = (base + 1 < lines.size()) ? lines[base + 1] : "";
      result[{refs[i].tid, refs[i].index}] = ParseAddrLineOutput(func, loc);
    }
  }

  return result;
}

// function formats multiple frame types across threads; splitting would reduce clarity.
// Conventional return type notation follows Google Style Guide.
// NOLINTNEXTLINE(readability-function-cognitive-complexity,modernize-use-trailing-return-type)
std::string FormatSymbolicated(const ParsedTombstone& tombstone, const SymbolTable& symbols) {
  static constexpr std::string_view kSep =
      "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***";

  std::ostringstream out;
  out << kSep << "\n";

  out << std::format("pid: {}, tid: {}, name: {}  >>> {} <<<\n", tombstone.pid,
                     tombstone.crashing_tid, tombstone.process_name, tombstone.process_name);

  // Signal line.
  const auto slash = tombstone.signal_info.find(" / ");
  const std::string sig_name =
      (slash != std::string::npos) ? tombstone.signal_info.substr(0, slash) : tombstone.signal_info;
  const std::string code_name =
      (slash != std::string::npos) ? tombstone.signal_info.substr(slash + 3) : "";

  if (code_name.empty()) {
    out << std::format("signal {} ({}), fault addr 0x{:016x}\n", tombstone.signal_number, sig_name,
                       tombstone.fault_addr);
  } else {
    out << std::format("signal {} ({}), code {} ({}), fault addr 0x{:016x}\n",
                       tombstone.signal_number, sig_name, tombstone.signal_code, code_name,
                       tombstone.fault_addr);
  }

  if (!tombstone.timestamp.empty()) {
    out << "timestamp: " << tombstone.timestamp << "\n";
  }

  auto emit_frames = [&](const ParsedThread& thread) {
    for (const auto& frame : thread.frames) {
      const auto sym_it = symbols.find({thread.tid, frame.index});
      const auto& mod =
          frame.module_path.empty() ? std::string_view{"???"} : std::string_view{frame.module_path};

      if (sym_it != symbols.end() && sym_it->second.function != "??") {
        const auto& sym = sym_it->second;
        if (!sym.source_file.empty() && sym.source_line > 0) {
          out << std::format("    #{:02d} pc 0x{:016x}  {} ({}) [{}:{}]\n", frame.index,
                             frame.module_offset, mod, sym.function, sym.source_file,
                             sym.source_line);
        } else {
          out << std::format("    #{:02d} pc 0x{:016x}  {} ({})\n", frame.index,
                             frame.module_offset, mod, sym.function);
        }
      } else if (frame.module_path.empty()) {
        out << std::format("    #{:02d} pc 0x{:016x}  ???\n", frame.index, frame.module_offset);
      } else if (!frame.trailing.empty()) {
        out << std::format("    #{:02d} pc 0x{:016x}  {} {}\n", frame.index, frame.module_offset,
                           frame.module_path, frame.trailing);
      } else {
        out << std::format("    #{:02d} pc 0x{:016x}  {}\n", frame.index, frame.module_offset,
                           frame.module_path);
      }
    }
  };

  // Crashing thread.
  if (!tombstone.threads.empty() && tombstone.threads[0].is_crashing) {
    out << "\nbacktrace:\n";
    emit_frames(tombstone.threads[0]);
  }

  // Other threads.
  for (size_t i = 1; i < tombstone.threads.size(); ++i) {
    const auto& thread = tombstone.threads[i];
    if (thread.name.empty()) {
      out << std::format("\n--- --- --- thread {} --- --- ---\n", thread.tid);
    } else {
      out << std::format("\n--- --- --- thread {} ({}) --- --- ---\n", thread.tid, thread.name);
    }
    emit_frames(thread);
  }

  if (!tombstone.minidump_path.empty()) {
    out << "\nminidump saved to: " << tombstone.minidump_path << "\n";
  }
  out << kSep << "\n";
  return out.str();
}

}  // namespace crashomon
