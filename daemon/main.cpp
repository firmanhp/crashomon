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
//
//   --db-path      Directory to watch / write minidumps to.
//                  Default: $CRASHOMON_DB_PATH or /var/crashomon.
//   --socket-path  Unix domain socket for crash handler connections.
//                  Default: $CRASHOMON_SOCKET_PATH or /run/crashomon/handler.sock.
//   --max-size     Maximum total size of .dmp files (e.g. 100M, 500K, 1G).
//                  0 or omitted = unlimited.
//   --max-age      Maximum age per file (e.g. 7d, 24h, 3600s).
//                  0 or omitted = unlimited.

#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "handler/linux/exception_handler_server.h"
#include "handler/linux/crash_report_exception_handler.h"
#include "client/crash_report_database.h"

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

// ── Crash handler hosting ─────────────────────────────────────────────────────

// Create, bind, and listen on a SOCK_SEQPACKET Unix domain socket.
// Returns the listening fd on success, or -1 on failure.
int CreateListenSocket(const std::string& socket_path) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  // Remove stale socket from a previous run.
  unlink(socket_path.c_str());

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("bind");
    close(fd);
    return -1;
  }

  // Allow all users to connect (monitored processes may run as different users).
  chmod(socket_path.c_str(), 0666);

  if (listen(fd, /*backlog=*/128) != 0) {
    perror("listen");
    close(fd);
    return -1;
  }

  return fd;
}

// Accept one client fd and run ExceptionHandlerServer on a detached thread.
// Thread exits when the client disconnects.
void AcceptAndHandleClient(
    int listen_fd,
    crashpad::CrashReportExceptionHandler* crash_handler) {
  int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (client_fd < 0) {
    if (errno != EINTR && errno != EAGAIN) {
      perror("accept4");
    }
    return;
  }

  // Enable credential passing so ExceptionHandlerServer can read the client PID
  // and call prctl(PR_SET_PTRACER) / ptrace-attach correctly.
  int one = 1;
  setsockopt(client_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

  std::thread([client_fd, crash_handler]() {
    crashpad::ExceptionHandlerServer server;
    if (server.InitializeWithClient(base::ScopedFD(client_fd),
                                    /*multiple_clients=*/true)) {
      server.Run(crash_handler);
    }
  }).detach();
}

// ── inotify + accept loop ─────────────────────────────────────────────────────

int RunWatcher(const std::string& db_path,
               const std::string& socket_path,
               const crashomon::DiskManagerConfig& prune_cfg) {
  // ── Crashpad handler setup ────────────────────────────────────────────────

  // Crashpad database lifecycle: writes to new/, then renames to pending/.
  // We watch pending/ for IN_MOVED_TO — the file is fully written and renamed
  // at that point. new/ is created by CrashReportDatabase::Initialize().
  std::string new_dir = db_path + "/new";
  std::string pending_dir = db_path + "/pending";
  {
    struct stat st;
    if (::stat(db_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      fprintf(stderr,
              "crashomon-watcherd: db-path is not a directory: %s\n",
              db_path.c_str());
      return 1;
    }
    // mkdir is best-effort; dirs may already exist after CrashReportDatabase::Initialize().
    mkdir(new_dir.c_str(), 0755);
    mkdir(pending_dir.c_str(), 0755);
  }

  auto database = crashpad::CrashReportDatabase::Initialize(
      base::FilePath(db_path));
  if (!database) {
    fprintf(stderr,
            "crashomon-watcherd: failed to initialize crash database at %s\n",
            db_path.c_str());
    return 1;
  }

  // crash data never leaves the device — no upload thread, no user stream sources.
  // process_annotations and attachments must be non-null (Crashpad dereferences them).
  static const std::map<std::string, std::string> kNoAnnotations;
  static const std::vector<base::FilePath> kNoAttachments;
  crashpad::CrashReportExceptionHandler crash_handler(
      database.get(),
      /*upload_thread=*/nullptr,
      &kNoAnnotations,
      &kNoAttachments,
      /*write_minidump_to_database=*/true,
      /*write_minidump_to_log=*/false,
      /*user_stream_data_sources=*/nullptr);

  int listen_fd = CreateListenSocket(socket_path);
  if (listen_fd < 0) {
    return 1;
  }
  fprintf(stderr, "crashomon-watcherd: listening on %s\n", socket_path.c_str());

  // ── inotify setup ─────────────────────────────────────────────────────────

  int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (ifd < 0) {
    perror("inotify_init1");
    close(listen_fd);
    return 1;
  }

  // Watch db_path/pending/ for IN_MOVED_TO — Crashpad writes minidumps to
  // new/<uuid>.dmp then atomically renames to pending/<uuid>.dmp.  The file is
  // fully written and ready to read only after the rename completes.
  int wd = inotify_add_watch(ifd, pending_dir.c_str(), IN_MOVED_TO);
  if (wd < 0) {
    perror("inotify_add_watch");
    close(ifd);
    close(listen_fd);
    return 1;
  }

  fprintf(stderr, "crashomon-watcherd: watching %s\n", pending_dir.c_str());

  // Buffer sized for 16 events with maximum-length names.
  constexpr size_t kBufSize = sizeof(struct inotify_event) + NAME_MAX + 1;
  char buf[kBufSize * 16];

  // Poll on both inotify fd and the listen socket.
  struct pollfd pfds[2];
  pfds[0].fd = ifd;
  pfds[0].events = POLLIN;
  pfds[1].fd = listen_fd;
  pfds[1].events = POLLIN;

  while (!g_stop) {
    int ready = poll(pfds, 2, /*timeout_ms=*/500);
    if (ready < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }
    if (ready == 0) continue;

    // ── New client connection ─────────────────────────────────────────────
    if (pfds[1].revents & POLLIN) {
      AcceptAndHandleClient(listen_fd, &crash_handler);
    }

    // ── New minidump written ──────────────────────────────────────────────
    if (!(pfds[0].revents & POLLIN)) continue;

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

      if (!(ev->mask & IN_MOVED_TO)) continue;
      if (ev->len == 0) continue;

      const char* name = ev->name;
      size_t namelen = strnlen(name, ev->len);
      if (namelen < 4 || strcmp(name + namelen - 4, ".dmp") != 0) continue;

      ProcessNewMinidump(pending_dir + "/" + name, prune_cfg);
    }
  }

  inotify_rm_watch(ifd, wd);
  close(ifd);
  close(listen_fd);
  unlink(socket_path.c_str());
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* db_path_env = getenv("CRASHOMON_DB_PATH");
  std::string db_path = db_path_env ? db_path_env : "/var/crashomon";

  const char* socket_path_env = getenv("CRASHOMON_SOCKET_PATH");
  std::string socket_path =
      socket_path_env ? socket_path_env : "/run/crashomon/handler.sock";

  uint64_t max_bytes = 0;
  uint32_t max_age_sec = 0;

  for (int i = 1; i < argc; ++i) {
    const char* v = nullptr;
    if ((v = GetArgValue(argv[i], "--db-path"))) {
      db_path = v;
    } else if ((v = GetArgValue(argv[i], "--socket-path"))) {
      socket_path = v;
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

  return RunWatcher(db_path, socket_path, prune_cfg);
}
