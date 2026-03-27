// crashomon-watcherd — crash watcher daemon
//
// Watches /var/crashomon (or CRASHOMON_DB_PATH) for new Breakpad minidumps
// written by crashpad_handler, parses them, and prints an Android-style
// tombstone to journald (or stderr).
//
// This file is a stub. Implementation is in Phase 2.

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
const char kDefaultDbPath[] = "/var/crashomon";
}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  const char* db_path = std::getenv("CRASHOMON_DB_PATH");
  if (!db_path) db_path = kDefaultDbPath;

  std::cerr << "crashomon-watcherd: stub — watching " << db_path << "\n"
            << "  (full implementation pending Phase 2)\n";
  return 1;
}
