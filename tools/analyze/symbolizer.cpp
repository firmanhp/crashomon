// tools/analyze/symbolizer.cpp — eu-addr2line-based frame symbolizer

#include "tools/analyze/symbolizer.h"

#include <sys/stat.h>
#include <cinttypes>
#include <cstdio>
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
    char hex[32];
    snprintf(hex, sizeof(hex), " 0x%016" PRIx64, r.offset);
    cmd += hex;
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
          {thread.tid, frame.index, frame.module_offset});
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

  char hdr[256];
  snprintf(hdr, sizeof(hdr),
           "pid: %" PRIu32 ", tid: %" PRIu32 ", name: %s  >>> %s <<<\n",
           tombstone.pid, tombstone.crashing_tid,
           tombstone.process_name.c_str(), tombstone.process_name.c_str());
  out << hdr;

  // Signal line.
  auto slash = tombstone.signal_info.find(" / ");
  std::string sig_name = (slash != std::string::npos)
      ? tombstone.signal_info.substr(0, slash) : tombstone.signal_info;
  std::string code_name = (slash != std::string::npos)
      ? tombstone.signal_info.substr(slash + 3) : "";

  char sig[256];
  if (code_name.empty()) {
    snprintf(sig, sizeof(sig),
             "signal %" PRIu32 " (%s), fault addr 0x%016" PRIx64 "\n",
             tombstone.signal_number, sig_name.c_str(), tombstone.fault_addr);
  } else {
    snprintf(sig, sizeof(sig),
             "signal %" PRIu32 " (%s), code %" PRIu32 " (%s), "
             "fault addr 0x%016" PRIx64 "\n",
             tombstone.signal_number, sig_name.c_str(),
             tombstone.signal_code, code_name.c_str(),
             tombstone.fault_addr);
  }
  out << sig;

  if (!tombstone.timestamp.empty()) {
    out << "timestamp: " << tombstone.timestamp << "\n";
  }

  auto emit_frames = [&](const ParsedThread& thread) {
    for (const auto& frame : thread.frames) {
      char line[512];
      auto it = symbols.find({thread.tid, frame.index});

      if (it != symbols.end() && it->second.function != "??") {
        const auto& sym = it->second;
        if (!sym.source_file.empty() && sym.source_line > 0) {
          snprintf(line, sizeof(line),
                   "    #%02d pc 0x%016" PRIx64 "  %s (%s) [%s:%d]\n",
                   frame.index, frame.module_offset,
                   frame.module_path.empty() ? "???" : frame.module_path.c_str(),
                   sym.function.c_str(), sym.source_file.c_str(),
                   sym.source_line);
        } else {
          snprintf(line, sizeof(line),
                   "    #%02d pc 0x%016" PRIx64 "  %s (%s)\n",
                   frame.index, frame.module_offset,
                   frame.module_path.empty() ? "???" : frame.module_path.c_str(),
                   sym.function.c_str());
        }
      } else if (frame.module_path.empty()) {
        snprintf(line, sizeof(line),
                 "    #%02d pc 0x%016" PRIx64 "  ???\n",
                 frame.index, frame.module_offset);
      } else {
        // No symbol — preserve original form with trailing info if present.
        if (!frame.trailing.empty()) {
          snprintf(line, sizeof(line),
                   "    #%02d pc 0x%016" PRIx64 "  %s %s\n",
                   frame.index, frame.module_offset,
                   frame.module_path.c_str(), frame.trailing.c_str());
        } else {
          snprintf(line, sizeof(line),
                   "    #%02d pc 0x%016" PRIx64 "  %s\n",
                   frame.index, frame.module_offset,
                   frame.module_path.c_str());
        }
      }
      out << line;
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
    char sep[128];
    if (t.name.empty()) {
      snprintf(sep, sizeof(sep),
               "\n--- --- --- thread %" PRIu32 " --- --- ---\n", t.tid);
    } else {
      snprintf(sep, sizeof(sep),
               "\n--- --- --- thread %" PRIu32 " (%s) --- --- ---\n",
               t.tid, t.name.c_str());
    }
    out << sep;
    emit_frames(t);
  }

  if (!tombstone.minidump_path.empty()) {
    out << "\nminidump saved to: " << tombstone.minidump_path << "\n";
  }
  out << kSep << "\n";
  return out.str();
}

}  // namespace crashomon
