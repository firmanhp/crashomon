// crashomon-analyze — minidump / tombstone symbolication tool
//
// Modes:
//   crashomon-analyze --store <dir> --minidump <file.dmp>
//       Symbolicate a Breakpad minidump using a symbol store.
//       Delegates to minidump_stackwalk; build IDs in the minidump auto-match
//       to .sym files in the store.
//
//   crashomon-analyze --store <dir> --stdin
//       Read a tombstone from stdin, symbolicate using eu-addr2line.
//       Module paths in the tombstone must be accessible on this machine.
//
//   crashomon-analyze --symbols <file.sym> --minidump <file.dmp>
//       Symbolicate a minidump using a single explicit .sym file.
//       The file is installed into a temporary Breakpad store layout and
//       minidump_stackwalk is invoked against it.
//
// Options:
//   --stackwalk-binary <path>   Path to minidump_stackwalk (default: find in PATH)
//   --addr2line-binary <path>   Path to eu-addr2line (default: eu-addr2line)

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "tombstone/minidump_reader.h"
#include "tombstone/tombstone_formatter.h"
#include "tools/analyze/log_parser.h"
#include "tools/analyze/minidump_analyzer.h"
#include "tools/analyze/symbolizer.h"

namespace {

// Match "--key=value" and return pointer to value, or nullptr if no match.
const char* GetArgValue(const char* arg, const char* key) {
  const size_t klen = strlen(key);
  if (strncmp(arg, key, klen) != 0) {
    return nullptr;
  }
  // C-string subscript and arithmetic on argv tokens; no cleaner alternative on raw char*.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (arg[klen] == '=') {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return arg + klen + 1;
  }
  return nullptr;
}

void Usage(const char* prog) {
  (void)std::fputs("Usage:\n", stderr);
  (void)std::fputs(("  " + std::string(prog) + " --store <dir> --minidump <file.dmp>\n").c_str(),
                   stderr);
  (void)std::fputs(("  " + std::string(prog) + " --store <dir> --stdin\n").c_str(), stderr);
  (void)std::fputs(
      ("  " + std::string(prog) + " --symbols <file.sym> --minidump <file.dmp>\n").c_str(), stderr);
  (void)std::fputs(("  " + std::string(prog) + " --minidump <file.dmp>\n").c_str(), stderr);
  (void)std::fputs(
      "\nOptions:\n"
      "  --stackwalk-binary <path>  (default: minidump_stackwalk)\n"
      "  --addr2line-binary <path>  (default: eu-addr2line)\n",
      stderr);
}

int ModeMinidumpStore(const std::string& store, const std::string& dump,
                      const std::string& stackwalk) {
  auto out_or = crashomon::RunMinidumpStackwalk(stackwalk, dump, {store});
  if (!out_or.ok()) {
    (void)std::fputs(
        ("crashomon-analyze: " + std::string(out_or.status().message()) + "\n").c_str(), stderr);
    return 1;
  }
  (void)std::fputs(out_or->c_str(), stdout);
  return 0;
}

// store/addr2line have semantically distinct roles; conventional return type notation is clearer
// per Google Style Guide.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int ModeStdinStore(const std::string& store, const std::string& addr2line) {
  // Read all of stdin.
  std::ostringstream buf;
  buf << std::cin.rdbuf();
  const std::string text = buf.str();

  auto tombstone_or = crashomon::ParseTombstone(text);
  if (!tombstone_or.ok()) {
    (void)std::fputs(
        ("crashomon-analyze: parse error: " + std::string(tombstone_or.status().message()) + "\n")
            .c_str(),
        stderr);
    return 1;
  }

  // Try eu-addr2line symbolication.
  auto syms_or = crashomon::SymbolizeWithAddrLine(*tombstone_or, addr2line);
  if (!syms_or.ok()) {
    (void)std::fputs(
        ("crashomon-analyze: symbolizer warning: " + std::string(syms_or.status().message()) +
         "\n  (continuing with unsymbolicated output)\n")
            .c_str(),
        stderr);
    const crashomon::SymbolTable empty;
    (void)std::fputs(crashomon::FormatSymbolicated(*tombstone_or, empty).c_str(), stdout);
    return 0;
  }

  (void)store;  // sym store not used in stdin+addr2line mode
  (void)std::fputs(crashomon::FormatSymbolicated(*tombstone_or, *syms_or).c_str(), stdout);
  return 0;
}

// Print raw (unsymbolicated) Android-style tombstone from a minidump.
int ModeRawTombstone(const std::string& dump) {
  auto info_or = crashomon::ReadMinidump(dump);
  if (!info_or.ok()) {
    (void)std::fputs(
        ("crashomon-analyze: " + std::string(info_or.status().message()) + "\n").c_str(), stderr);
    return 1;
  }
  (void)std::fputs(crashomon::FormatTombstone(*info_or).c_str(), stdout);
  return 0;
}

int ModeSymFileMinidump(const std::string& sym_file, const std::string& dump,
                        const std::string& stackwalk) {
  auto out_or = crashomon::RunWithSingleSymFile(stackwalk, sym_file, dump);
  if (!out_or.ok()) {
    (void)std::fputs(
        ("crashomon-analyze: " + std::string(out_or.status().message()) + "\n").c_str(), stderr);
    return 1;
  }
  (void)std::fputs(out_or->c_str(), stdout);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string store;
  std::string minidump;
  std::string sym_file;
  std::string stackwalk = "minidump_stackwalk";
  std::string addr2line = "eu-addr2line";
  bool read_stdin = false;

  for (int idx = 1; idx < argc; ++idx) {
    const char* arg = argv[idx];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) — argv
                                  // subscript; standard C main() access pattern.
    const char* arg_val = GetArgValue(arg, "--store");
    if (arg_val != nullptr) {
      store = arg_val;
      continue;
    }
    arg_val = GetArgValue(arg, "--minidump");
    if (arg_val != nullptr) {
      minidump = arg_val;
      continue;
    }
    arg_val = GetArgValue(arg, "--symbols");
    if (arg_val != nullptr) {
      sym_file = arg_val;
      continue;
    }
    arg_val = GetArgValue(arg, "--stackwalk-binary");
    if (arg_val != nullptr) {
      stackwalk = arg_val;
      continue;
    }
    arg_val = GetArgValue(arg, "--addr2line-binary");
    if (arg_val != nullptr) {
      addr2line = arg_val;
      continue;
    }
    if (strcmp(arg, "--stdin") == 0) {
      read_stdin = true;
      continue;
    }
    (void)std::fputs(("crashomon-analyze: unknown argument: " + std::string(arg) + "\n").c_str(),
                     stderr);
    Usage(argv[0]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) — argv[0] is the
                     // program name; standard C main() pattern.
    return 1;
  }

  // Dispatch to the appropriate mode.
  if (!store.empty() && !minidump.empty()) {
    return ModeMinidumpStore(store, minidump, stackwalk);
  }
  if (!store.empty() && read_stdin) {
    return ModeStdinStore(store, addr2line);
  }
  if (!sym_file.empty() && !minidump.empty()) {
    return ModeSymFileMinidump(sym_file, minidump, stackwalk);
  }
  if (!minidump.empty()) {
    return ModeRawTombstone(minidump);
  }

  (void)std::fputs("crashomon-analyze: no valid mode specified.\n", stderr);
  Usage(argv[0]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) — argv[0] is the
                   // program name; standard C main() pattern.
  return 1;
}
