// tools/analyze/symbolizer.cpp — eu-addr2line-based frame symbolizer

#include "tools/analyze/symbolizer.h"

#include <sys/stat.h>
#include <cstdio>
#include <format>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
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
std::string RunCapture(const std::string& cmd) {
  FILE* fp = popen(cmd.c_str(), "r");  // NOLINT(cert-env33-c)
  if (!fp) return "";
  std::string out;
  char buf[256];
  while (fgets(buf, sizeof(buf), fp)) out += buf;
  int rc = pclose(fp);
  if (rc != 0) return "";
  return out;
}

// Escape a path so it is safe to embed in a shell command.
// We only allow paths that contain no shell-special characters;
// anything else is wrapped in single quotes with embedded ' escaped.
std::string ShellQuote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else            out += c;
  }
  out += "'";
  return out;
}

bool FileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Call addr2line once for all offsets belonging to one module.
// Returns lines in order: 2 lines per address (function, then file:line).
std::vector<std::string> QueryAddrLine(const std::string& binary,
                                        const std::string& addr2line,
                                        const std::vector<FrameRef>& refs) {
  if (refs.empty()) return {};

  // Build: addr2line -e <module> -f -s -p <offset1> <offset2> ...
  // -f: print function name  -s: strip directory  -p: pretty-print (one line)
  // We use plain -f (two lines per addr) for simpler parsing.
  std::string cmd = ShellQuote(addr2line) + " -e " + ShellQuote(binary) + " -f -s";
  for (const auto& r : refs) {
    cmd += std::format(" 0x{:016x}", r.offset);
  }
  cmd += " 2>/dev/null";

  std::string raw = RunCapture(cmd);
  std::vector<std::string> lines;
  std::istringstream ss(raw);
  std::string line;
  while (std::getline(ss, line)) lines.push_back(line);
  return lines;
}

SymbolInfo ParseAddrLineOutput(const std::string& func_line,
                                const std::string& loc_line) {
  SymbolInfo sym;
  sym.function = func_line;
  if (sym.function.empty()) sym.function = "??";

  // loc_line is "file.c:142" or "??:0"
  auto colon = loc_line.rfind(':');
  if (colon != std::string::npos) {
    sym.source_file = loc_line.substr(0, colon);
    const std::string num_str = loc_line.substr(colon + 1);
    char* end = nullptr;
    long val = std::strtol(num_str.c_str(), &end, 10);
    sym.source_line = (end != num_str.c_str()) ? static_cast<int>(val) : 0;
  }
  if (sym.source_file == "??") sym.source_file.clear();
  return sym;
}

}  // namespace

absl::StatusOr<SymbolTable> SymbolizeWithAddrLine(
    const ParsedTombstone& tombstone, std::string_view addr2line_binary) {
  // Group frames by module path.
  std::map<std::string, std::vector<FrameRef>> by_module;
  for (const auto& thread : tombstone.threads) {
    for (const auto& frame : thread.frames) {
      if (frame.module_path.empty()) continue;
      by_module[frame.module_path].push_back(
          {.tid = thread.tid, .index = frame.index, .offset = frame.module_offset});
    }
  }

  SymbolTable result;
  std::string binary(addr2line_binary);

  for (const auto& [module, refs] : by_module) {
    if (!FileExists(module)) {
      // Binary not found on this machine — mark frames as unknown.
      for (const auto& r : refs) {
        result[{r.tid, r.index}] = {"??", "", 0};
      }
      continue;
    }

    std::vector<std::string> lines = QueryAddrLine(module, binary, refs);

    // Expect 2 lines per address.
    for (size_t i = 0; i < refs.size(); ++i) {
      size_t base = i * 2;
      std::string func = (base < lines.size()) ? lines[base] : "??";
      std::string loc  = (base + 1 < lines.size()) ? lines[base + 1] : "";
      result[{refs[i].tid, refs[i].index}] =
          ParseAddrLineOutput(func, loc);
    }
  }

  return result;
}

std::string FormatSymbolicated(const ParsedTombstone& tombstone,
                                const SymbolTable& symbols) {
  static constexpr const char kSep[] =
      "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***";

  std::ostringstream out;
  out << kSep << "\n";

  out << std::format("pid: {}, tid: {}, name: {}  >>> {} <<<\n",
                     tombstone.pid, tombstone.crashing_tid,
                     tombstone.process_name, tombstone.process_name);

  // Signal line.
  auto slash = tombstone.signal_info.find(" / ");
  std::string sig_name = (slash != std::string::npos)
      ? tombstone.signal_info.substr(0, slash) : tombstone.signal_info;
  std::string code_name = (slash != std::string::npos)
      ? tombstone.signal_info.substr(slash + 3) : "";

  if (code_name.empty()) {
    out << std::format("signal {} ({}), fault addr 0x{:016x}\n",
                       tombstone.signal_number, sig_name, tombstone.fault_addr);
  } else {
    out << std::format("signal {} ({}), code {} ({}), fault addr 0x{:016x}\n",
                       tombstone.signal_number, sig_name,
                       tombstone.signal_code, code_name, tombstone.fault_addr);
  }

  if (!tombstone.timestamp.empty()) {
    out << "timestamp: " << tombstone.timestamp << "\n";
  }

  auto emit_frames = [&](const ParsedThread& thread) {
    for (const auto& frame : thread.frames) {
      auto it = symbols.find({thread.tid, frame.index});
      const auto& mod = frame.module_path.empty()
          ? std::string_view{"???"} : std::string_view{frame.module_path};

      if (it != symbols.end() && it->second.function != "??") {
        const auto& sym = it->second;
        if (!sym.source_file.empty() && sym.source_line > 0) {
          out << std::format("    #{:02d} pc 0x{:016x}  {} ({}) [{}:{}]\n",
                             frame.index, frame.module_offset,
                             mod, sym.function, sym.source_file, sym.source_line);
        } else {
          out << std::format("    #{:02d} pc 0x{:016x}  {} ({})\n",
                             frame.index, frame.module_offset, mod, sym.function);
        }
      } else if (frame.module_path.empty()) {
        out << std::format("    #{:02d} pc 0x{:016x}  ???\n",
                           frame.index, frame.module_offset);
      } else if (!frame.trailing.empty()) {
        out << std::format("    #{:02d} pc 0x{:016x}  {} {}\n",
                           frame.index, frame.module_offset,
                           frame.module_path, frame.trailing);
      } else {
        out << std::format("    #{:02d} pc 0x{:016x}  {}\n",
                           frame.index, frame.module_offset, frame.module_path);
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
    const auto& t = tombstone.threads[i];
    if (t.name.empty()) {
      out << std::format("\n--- --- --- thread {} --- --- ---\n", t.tid);
    } else {
      out << std::format("\n--- --- --- thread {} ({}) --- --- ---\n",
                         t.tid, t.name);
    }
    emit_frames(t);
  }

  if (!tombstone.minidump_path.empty()) {
    out << "\nminidump saved to: " << tombstone.minidump_path << "\n";
  }
  out << kSep << "\n";
  return out.str();
}

}  // namespace crashomon
