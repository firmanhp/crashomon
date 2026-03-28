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

#include "tools/analyze/log_parser.h"
#include "tools/analyze/minidump_analyzer.h"
#include "tools/analyze/symbolizer.h"

namespace {

const char* GetArgValue(const char* arg, const char* key) {
  size_t klen = strlen(key);
  if (strncmp(arg, key, klen) != 0) return nullptr;
  if (arg[klen] == '=') return arg + klen + 1;
  return nullptr;
}

void Usage(const char* prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s --store <dir> --minidump <file.dmp>\n"
          "  %s --store <dir> --stdin\n"
          "  %s --symbols <file.sym> --minidump <file.dmp>\n"
          "\n"
          "Options:\n"
          "  --stackwalk-binary <path>  (default: minidump_stackwalk)\n"
          "  --addr2line-binary <path>  (default: eu-addr2line)\n",
          prog, prog, prog);
}

int ModeMinidumpStore(const std::string& store, const std::string& dump,
                      const std::string& stackwalk) {
  auto out_or = crashomon::RunMinidumpStackwalk(stackwalk, dump, {store});
  if (!out_or.ok()) {
    fprintf(stderr, "crashomon-analyze: %s\n",
            out_or.status().message().data());
    return 1;
  }
  fputs(out_or->c_str(), stdout);
  return 0;
}

int ModeStdinStore(const std::string& store, const std::string& addr2line) {
  // Read all of stdin.
  std::ostringstream buf;
  buf << std::cin.rdbuf();
  std::string text = buf.str();

  auto tombstone_or = crashomon::ParseTombstone(text);
  if (!tombstone_or.ok()) {
    fprintf(stderr, "crashomon-analyze: parse error: %s\n",
            tombstone_or.status().message().data());
    return 1;
  }

  // Try eu-addr2line symbolication.
  auto syms_or = crashomon::SymbolizeWithAddrLine(*tombstone_or, addr2line);
  if (!syms_or.ok()) {
    fprintf(stderr, "crashomon-analyze: symbolizer warning: %s\n"
                    "  (continuing with unsymbolicated output)\n",
            syms_or.status().message().data());
    crashomon::SymbolTable empty;
    fputs(crashomon::FormatSymbolicated(*tombstone_or, empty).c_str(), stdout);
    return 0;
  }

  (void)store; // sym store not used in stdin+addr2line mode
  fputs(crashomon::FormatSymbolicated(*tombstone_or, *syms_or).c_str(),
        stdout);
  return 0;
}

int ModeSymFileMinidump(const std::string& sym_file, const std::string& dump,
                         const std::string& stackwalk) {
  auto out_or = crashomon::RunWithSingleSymFile(stackwalk, sym_file, dump);
  if (!out_or.ok()) {
    fprintf(stderr, "crashomon-analyze: %s\n",
            out_or.status().message().data());
    return 1;
  }
  fputs(out_or->c_str(), stdout);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string store, minidump, sym_file;
  std::string stackwalk = "minidump_stackwalk";
  std::string addr2line = "eu-addr2line";
  bool read_stdin = false;

  for (int i = 1; i < argc; ++i) {
    const char* v = nullptr;
    if ((v = GetArgValue(argv[i], "--store")))            { store = v; }
    else if ((v = GetArgValue(argv[i], "--minidump")))    { minidump = v; }
    else if ((v = GetArgValue(argv[i], "--symbols")))     { sym_file = v; }
    else if ((v = GetArgValue(argv[i], "--stackwalk-binary"))) { stackwalk = v; }
    else if ((v = GetArgValue(argv[i], "--addr2line-binary"))) { addr2line = v; }
    else if (strcmp(argv[i], "--stdin") == 0) { read_stdin = true; }
    else {
      fprintf(stderr, "crashomon-analyze: unknown argument: %s\n", argv[i]);
      Usage(argv[0]);
      return 1;
    }
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

  fprintf(stderr, "crashomon-analyze: no valid mode specified.\n");
  Usage(argv[0]);
  return 1;
}
