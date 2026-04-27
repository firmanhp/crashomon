// crashomon-watcherd — crash watcher daemon
//
// Watches a directory for new Breakpad minidumps written by crashpad_handler,
// parses them with the Breakpad processor, and prints an Android-style tombstone
// to stderr (captured by journald when running as a systemd service).
//
// Also hosts the Crashpad ExceptionHandlerServer directly, replacing the need for
// a separate crashpad_handler process.  Client processes connect via a Unix domain
// socket; watcherd ptrace-attaches to capture the minidump in-process.
//
// Usage:
//   crashomon-watcherd [--db-path=PATH] [--socket-path=PATH]
//                      [--max-size=SIZE] [--max-age=AGE]
//                      [--export-path=PATH]
//                      [--patch-build-ids=auto|never]
//
//   --db-path          Directory to watch / write minidumps to.
//                      Default: $CRASHOMON_DB_PATH or /var/crashomon.
//   --socket-path      Unix domain socket for crash handler connections.
//                      Default: $CRASHOMON_SOCKET_PATH or /run/crashomon/handler.sock.
//   --max-size         Maximum total size of .dmp files (e.g. 100M, 500K, 1G).
//                      0 or omitted = unlimited.
//   --max-age          Maximum age per file (e.g. 7d, 24h, 3600s).
//                      0 or omitted = unlimited.
//   --export-path      Directory to copy each new minidump into on arrival.
//                      Files are named: {process}_{build_id8}_{YYYYMMDDHHmmss}.crashdump
//                      Pruned by the same --max-size/--max-age limits as the db.
//                      Default: $CRASHOMON_EXPORT_PATH or disabled.
//   --patch-build-ids  'auto' (default) or 'never': whether to patch missing BpEL
//                      build IDs in each new minidump before processing.

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "daemon/disk_manager.h"
#include "daemon/log.h"
#include "daemon/watcher.h"
#include "spdlog/spdlog.h"

namespace {

// ── Named constants ───────────────────────────────────────────────────────────

constexpr uint64_t kBytesPerKilobyte = 1024ULL;
constexpr uint32_t kSecondsPerHour = 3600U;
constexpr uint32_t kSecondsPerDay = 86400U;

// ── Argument parsing ──────────────────────────────────────────────────────────

// Parse a size string with optional suffix (K/M/G) and return bytes.
// Returns 0 on error.
uint64_t ParseSize(const char* str) {
  if (str == nullptr || *str == '\0') {
    return 0;
  }
  char* end = nullptr;
  const unsigned long long val = strtoull(str, &end, 10);
  if (end == str) {
    return 0;
  }
  if (*end == '\0' || strcmp(end, "B") == 0) {
    return static_cast<uint64_t>(val);
  }
  if (strcmp(end, "K") == 0 || strcmp(end, "KB") == 0) {
    return static_cast<uint64_t>(val) * kBytesPerKilobyte;
  }
  if (strcmp(end, "M") == 0 || strcmp(end, "MB") == 0) {
    return static_cast<uint64_t>(val) * kBytesPerKilobyte * kBytesPerKilobyte;
  }
  if (strcmp(end, "G") == 0 || strcmp(end, "GB") == 0) {
    return static_cast<uint64_t>(val) * kBytesPerKilobyte * kBytesPerKilobyte * kBytesPerKilobyte;
  }
  return 0;
}

// Parse a duration string with optional suffix (s/h/d) and return seconds.
// Returns 0 on error.
uint32_t ParseAge(const char* str) {
  if (str == nullptr || *str == '\0') {
    return 0;
  }
  char* end = nullptr;
  const unsigned long val = strtoul(str, &end, 10);
  if (end == str) {
    return 0;
  }
  if (*end == '\0' || strcmp(end, "s") == 0) {
    return static_cast<uint32_t>(val);
  }
  if (strcmp(end, "h") == 0) {
    return static_cast<uint32_t>(val) * kSecondsPerHour;
  }
  if (strcmp(end, "d") == 0) {
    return static_cast<uint32_t>(val) * kSecondsPerDay;
  }
  return 0;
}

// Match "--key=value" and return pointer to value, or nullptr if no match.
const char* GetArgValue(const char* arg, const char* key) {
  const size_t key_len = strlen(key);
  if (strncmp(arg, key, key_len) != 0) {
    return nullptr;
  }
  // argv/C-string pointer arithmetic; no safe alternative without a third-party span library.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (arg[key_len] == '=') {
    return arg + key_len + 1;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return nullptr;
}

}  // namespace

int main(int argc, char* argv[]) {
  crashomon::InitLogging();

  const char* db_path_env = getenv("CRASHOMON_DB_PATH");
  std::string db_path = (db_path_env != nullptr) ? db_path_env : "/var/crashomon";

  const char* socket_path_env = getenv("CRASHOMON_SOCKET_PATH");
  std::string socket_path =
      (socket_path_env != nullptr) ? socket_path_env : "/run/crashomon/handler.sock";

  const char* export_path_env = getenv("CRASHOMON_EXPORT_PATH");
  std::string export_path = (export_path_env != nullptr) ? export_path_env : "";

  uint64_t max_bytes = 0;
  uint32_t max_age_sec = 0;
  bool patch_build_ids = true;

  // argv is a C array; pointer arithmetic is the only way to construct a range from it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  const std::vector<std::string_view> args{argv + 1, argv + argc};
  for (const auto& arg_sv : args) {
    const char* arg = arg_sv.data();
    const char* arg_val = nullptr;
    if (arg_val = GetArgValue(arg, "--db-path"); arg_val != nullptr) {
      db_path = arg_val;
    } else if (arg_val = GetArgValue(arg, "--socket-path"); arg_val != nullptr) {
      socket_path = arg_val;
    } else if (arg_val = GetArgValue(arg, "--max-size"); arg_val != nullptr) {
      max_bytes = ParseSize(arg_val);
    } else if (arg_val = GetArgValue(arg, "--max-age"); arg_val != nullptr) {
      max_age_sec = ParseAge(arg_val);
    } else if (arg_val = GetArgValue(arg, "--export-path"); arg_val != nullptr) {
      export_path = arg_val;
    } else if (arg_val = GetArgValue(arg, "--patch-build-ids"); arg_val != nullptr) {
      if (strcmp(arg_val, "never") == 0) {
        patch_build_ids = false;
      } else if (strcmp(arg_val, "auto") != 0) {
        spdlog::error("crashomon-watcherd: --patch-build-ids must be 'auto' or 'never'");
        return 1;
      }
    } else {
      spdlog::error("crashomon-watcherd: unknown argument: {}", arg);
      return 1;
    }
  }

  crashomon::SetupSignalHandlers();

  crashomon::DiskManagerConfig prune_cfg;
  prune_cfg.db_path = db_path;
  prune_cfg.export_path = export_path;
  prune_cfg.max_bytes = max_bytes;
  prune_cfg.max_age_seconds = max_age_sec;

  return crashomon::RunWatcher(db_path, socket_path, prune_cfg, export_path, patch_build_ids);
}
