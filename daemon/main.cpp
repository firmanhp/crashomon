// crashomon-watcherd — crash watcher daemon
//
// Watches a directory for new Breakpad minidumps written by crashpad_handler,
// parses them with the Breakpad processor, and prints an Android-style tombstone
// to stderr (captured by journald when running as a systemd service).
//
// Usage:
//   crashomon-watcherd [--db-path=PATH] [--max-size=SIZE] [--max-age=AGE]
//
//   --db-path   Directory to watch. Default: $CRASHOMON_DB_PATH or /var/crashomon.
//   --max-size  Maximum total size of .dmp files (e.g. 100M, 500K, 1G).
//               0 or omitted = unlimited.
//   --max-age   Maximum age per file (e.g. 7d, 24h, 3600s).
//               0 or omitted = unlimited.

#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "daemon/disk_manager.h"
#include "daemon/minidump_reader.h"
#include "daemon/tombstone_formatter.h"

namespace {

static volatile sig_atomic_t g_stop = 0;

void HandleSignal(int /*sig*/) { g_stop = 1; }

// ── Argument parsing ──────────────────────────────────────────────────────────

// Parse a size string with optional suffix (K/M/G) and return bytes.
// Returns 0 on error.
uint64_t ParseSize(const char* s) {
  if (!s || *s == '\0') return 0;
  char* end = nullptr;
  unsigned long long val = strtoull(s, &end, 10);
  if (end == s) return 0;
  if (*end == '\0' || strcmp(end, "B") == 0) return static_cast<uint64_t>(val);
  if (strcmp(end, "K") == 0 || strcmp(end, "KB") == 0)
    return static_cast<uint64_t>(val) * 1024ULL;
  if (strcmp(end, "M") == 0 || strcmp(end, "MB") == 0)
    return static_cast<uint64_t>(val) * 1024ULL * 1024ULL;
  if (strcmp(end, "G") == 0 || strcmp(end, "GB") == 0)
    return static_cast<uint64_t>(val) * 1024ULL * 1024ULL * 1024ULL;
  return 0;
}

// Parse a duration string with optional suffix (s/h/d) and return seconds.
// Returns 0 on error.
uint32_t ParseAge(const char* s) {
  if (!s || *s == '\0') return 0;
  char* end = nullptr;
  unsigned long val = strtoul(s, &end, 10);
  if (end == s) return 0;
  if (*end == '\0' || strcmp(end, "s") == 0) return static_cast<uint32_t>(val);
  if (strcmp(end, "h") == 0) return static_cast<uint32_t>(val) * 3600U;
  if (strcmp(end, "d") == 0) return static_cast<uint32_t>(val) * 86400U;
  return 0;
}

// Match "--key=value" and return pointer to value, or nullptr if no match.
const char* GetArgValue(const char* arg, const char* key) {
  size_t klen = strlen(key);
  if (strncmp(arg, key, klen) != 0) return nullptr;
  if (arg[klen] == '=') return arg + klen + 1;
  return nullptr;
}

// ── Minidump processing ───────────────────────────────────────────────────────

void ProcessNewMinidump(const std::string& path,
                        const crashomon::DiskManagerConfig& prune_cfg) {
  auto info_or = crashomon::ReadMinidump(path);
  if (!info_or.ok()) {
    fprintf(stderr, "crashomon-watcherd: failed to read '%s': %s\n",
            path.c_str(), info_or.status().message().data());
    return;
  }

  std::string tombstone = crashomon::FormatTombstone(*info_or);
  // Single fwrite keeps the tombstone atomic in journald's view.
  fwrite(tombstone.data(), 1, tombstone.size(), stderr);
  fflush(stderr);

  auto prune_status = crashomon::PruneMinidumps(prune_cfg);
  if (!prune_status.ok()) {
    fprintf(stderr, "crashomon-watcherd: pruning error: %s\n",
            prune_status.message().data());
  }
}

// ── inotify loop ─────────────────────────────────────────────────────────────

int RunWatcher(const std::string& db_path,
               const crashomon::DiskManagerConfig& prune_cfg) {
  struct stat st;
  if (::stat(db_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    fprintf(stderr,
            "crashomon-watcherd: db-path is not a directory: %s\n",
            db_path.c_str());
    return 1;
  }

  int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (ifd < 0) {
    perror("inotify_init1");
    return 1;
  }

  // IN_CLOSE_WRITE: file closed after writing (Crashpad write-then-close).
  // IN_MOVED_TO:    atomic rename-into-dir pattern.
  int wd = inotify_add_watch(ifd, db_path.c_str(),
                              IN_CLOSE_WRITE | IN_MOVED_TO);
  if (wd < 0) {
    perror("inotify_add_watch");
    close(ifd);
    return 1;
  }

  fprintf(stderr, "crashomon-watcherd: watching %s\n", db_path.c_str());

  // Buffer sized for 16 events with maximum-length names.
  constexpr size_t kBufSize = sizeof(struct inotify_event) + NAME_MAX + 1;
  char buf[kBufSize * 16];

  struct pollfd pfd;
  pfd.fd = ifd;
  pfd.events = POLLIN;

  while (!g_stop) {
    int ready = poll(&pfd, 1, /*timeout_ms=*/500);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }
    if (ready == 0) continue;

    ssize_t n = read(ifd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      perror("read(inotify)");
      break;
    }

    for (ssize_t offset = 0; offset < n; ) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      auto* ev = reinterpret_cast<struct inotify_event*>(buf + offset);
      offset += static_cast<ssize_t>(sizeof(struct inotify_event) +
                                     ev->len);

      if (!(ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))) continue;
      if (ev->len == 0) continue;

      const char* name = ev->name;
      size_t namelen = strnlen(name, ev->len);
      if (namelen < 4 || strcmp(name + namelen - 4, ".dmp") != 0) continue;

      ProcessNewMinidump(db_path + "/" + name, prune_cfg);
    }
  }

  inotify_rm_watch(ifd, wd);
  close(ifd);
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* db_path_env = getenv("CRASHOMON_DB_PATH");
  std::string db_path = db_path_env ? db_path_env : "/var/crashomon";
  uint64_t max_bytes = 0;
  uint32_t max_age_sec = 0;

  for (int i = 1; i < argc; ++i) {
    const char* v = nullptr;
    if ((v = GetArgValue(argv[i], "--db-path"))) {
      db_path = v;
    } else if ((v = GetArgValue(argv[i], "--max-size"))) {
      max_bytes = ParseSize(v);
    } else if ((v = GetArgValue(argv[i], "--max-age"))) {
      max_age_sec = ParseAge(v);
    } else {
      fprintf(stderr, "crashomon-watcherd: unknown argument: %s\n", argv[i]);
      return 1;
    }
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = HandleSignal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  crashomon::DiskManagerConfig prune_cfg;
  prune_cfg.db_path = db_path;
  prune_cfg.max_bytes = max_bytes;
  prune_cfg.max_age_seconds = max_age_sec;

  return RunWatcher(db_path, prune_cfg);
}
