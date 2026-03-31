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
#include <csignal>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "handler/linux/exception_handler_server.h"
#include "handler/linux/crash_report_exception_handler.h"
#include "client/crash_report_database.h"

#include "daemon/disk_manager.h"
#include "daemon/minidump_reader.h"
#include "daemon/tombstone_formatter.h"

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t g_stop = 0;

void HandleSignal(int /*sig*/) { g_stop = 1; }

// ── Logging ───────────────────────────────────────────────────────────────────

void Log(std::string_view msg) {
  (void)std::fwrite(msg.data(), 1, msg.size(), stderr);
  (void)std::fputc('\n', stderr);
  (void)std::fflush(stderr);
}

// ── Argument parsing ──────────────────────────────────────────────────────────

// Parse a size string with optional suffix (K/M/G) and return bytes.
// Returns 0 on error.
uint64_t ParseSize(const char* str) {
  if (str == nullptr || *str == '\0') { return 0; }
  char* end = nullptr;
  const unsigned long long val = strtoull(str, &end, 10);
  if (end == str) { return 0; }
  if (*end == '\0' || strcmp(end, "B") == 0) { return static_cast<uint64_t>(val); }
  if (strcmp(end, "K") == 0 || strcmp(end, "KB") == 0) {
    return static_cast<uint64_t>(val) * 1024ULL;
  }
  if (strcmp(end, "M") == 0 || strcmp(end, "MB") == 0) {
    return static_cast<uint64_t>(val) * 1024ULL * 1024ULL;
  }
  if (strcmp(end, "G") == 0 || strcmp(end, "GB") == 0) {
    return static_cast<uint64_t>(val) * 1024ULL * 1024ULL * 1024ULL;
  }
  return 0;
}

// Parse a duration string with optional suffix (s/h/d) and return seconds.
// Returns 0 on error.
uint32_t ParseAge(const char* str) {
  if (str == nullptr || *str == '\0') { return 0; }
  char* end = nullptr;
  const unsigned long val = strtoul(str, &end, 10);
  if (end == str) { return 0; }
  if (*end == '\0' || strcmp(end, "s") == 0) { return static_cast<uint32_t>(val); }
  if (strcmp(end, "h") == 0) { return static_cast<uint32_t>(val) * 3600U; }
  if (strcmp(end, "d") == 0) { return static_cast<uint32_t>(val) * 86400U; }
  return 0;
}

// Match "--key=value" and return pointer to value, or nullptr if no match.
const char* GetArgValue(const char* arg, const char* key) {
  const size_t key_len = strlen(key);
  if (strncmp(arg, key, key_len) != 0) { return nullptr; }
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (arg[key_len] == '=') {
    return arg + key_len + 1;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  return nullptr;
}

// ── Minidump processing ───────────────────────────────────────────────────────

void ProcessNewMinidump(const std::string& path,
                        const crashomon::DiskManagerConfig& prune_cfg) {
  auto info_or = crashomon::ReadMinidump(path);
  if (!info_or.ok()) {
    Log(std::string{"crashomon-watcherd: failed to read '"} + path + "': " +
        info_or.status().message().data());
    return;
  }

  std::string tombstone = crashomon::FormatTombstone(*info_or);
  // Single fwrite keeps the tombstone atomic in journald's view.
  (void)fwrite(tombstone.data(), 1, tombstone.size(), stderr);
  (void)fflush(stderr);

  auto prune_status = crashomon::PruneMinidumps(prune_cfg);
  if (!prune_status.ok()) {
    Log(std::string{"crashomon-watcherd: pruning error: "} +
        prune_status.message().data());
  }
}

// ── Crash handler hosting ─────────────────────────────────────────────────────

// Create, bind, and listen on a SOCK_SEQPACKET Unix domain socket.
// Returns the listening fd on success, or -1 on failure.
int CreateListenSocket(const std::string& socket_path) {
  const int sock_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock_fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  // Remove stale socket from a previous run.
  (void)unlink(socket_path.c_str());

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (bind(sock_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("bind");
    (void)close(sock_fd);
    return -1;
  }

  // Allow all users to connect (monitored processes may run as different users).
  (void)chmod(socket_path.c_str(), 0666);

  if (listen(sock_fd, /*backlog=*/128) != 0) {
    perror("listen");
    (void)close(sock_fd);
    return -1;
  }

  return sock_fd;
}

// Accept one client fd and run ExceptionHandlerServer on a detached thread.
// Thread exits when the client disconnects.
void AcceptAndHandleClient(
    int listen_fd,
    crashpad::CrashReportExceptionHandler* crash_handler) {
  const int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (client_fd < 0) {
    if (errno != EINTR && errno != EAGAIN) {
      perror("accept4");
    }
    return;
  }

  // Enable credential passing so ExceptionHandlerServer can read the client PID
  // and call prctl(PR_SET_PTRACER) / ptrace-attach correctly.
  int one = 1;
  (void)setsockopt(client_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

  std::thread([client_fd, crash_handler]() {
    crashpad::ExceptionHandlerServer server;
    if (server.InitializeWithClient(base::ScopedFD(client_fd),
                                    /*multiple_clients=*/true)) {
      server.Run(crash_handler);
    }
  }).detach();
}

// ── inotify event processing ──────────────────────────────────────────────────

// Process a raw inotify read buffer, triggering minidump handling for each
// IN_MOVED_TO event on a .dmp file.
void ProcessInotifyEvents(std::string_view pending_dir,
                          const crashomon::DiskManagerConfig& prune_cfg,
                          const char* buf, ssize_t count) {
  for (ssize_t offset = 0; offset < count; ) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* event = reinterpret_cast<const struct inotify_event*>(buf + offset);
    offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);

    if ((event->mask & IN_MOVED_TO) == 0) { continue; }
    if (event->len == 0) { continue; }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* name = event->name;
    const size_t name_len = strnlen(name, event->len);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (name_len < 4 || strcmp(name + name_len - 4, ".dmp") != 0) { continue; }

    ProcessNewMinidump(std::string{pending_dir} + "/" + name, prune_cfg);
  }
}

// ── inotify + accept loop ─────────────────────────────────────────────────────

int RunWatcher(const std::string& db_path,
               const std::string& socket_path,
               const crashomon::DiskManagerConfig& prune_cfg) {
  // ── Crashpad handler setup ────────────────────────────────────────────────

  // Crashpad database lifecycle: writes to new/, then renames to pending/.
  // We watch pending/ for IN_MOVED_TO — the file is fully written and renamed
  // at that point. new/ is created by CrashReportDatabase::Initialize().
  const std::string new_dir = db_path + "/new";
  const std::string pending_dir = db_path + "/pending";
  {
    struct stat stat_buf{};
    if (::stat(db_path.c_str(), &stat_buf) != 0 || !S_ISDIR(stat_buf.st_mode)) {
      Log(std::string{"crashomon-watcherd: db-path is not a directory: "} + db_path);
      return 1;
    }
    // mkdir is best-effort; dirs may already exist after CrashReportDatabase::Initialize().
    (void)mkdir(new_dir.c_str(), 0755);
    (void)mkdir(pending_dir.c_str(), 0755);
  }

  auto database = crashpad::CrashReportDatabase::Initialize(
      base::FilePath(db_path));
  if (database == nullptr) {
    Log(std::string{"crashomon-watcherd: failed to initialize crash database at "} +
        db_path);
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

  const int listen_fd = CreateListenSocket(socket_path);
  if (listen_fd < 0) {
    return 1;
  }
  Log(std::string{"crashomon-watcherd: listening on "} + socket_path);

  // ── inotify setup ─────────────────────────────────────────────────────────

  const int ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (ifd < 0) {
    perror("inotify_init1");
    (void)close(listen_fd);
    return 1;
  }

  // Watch db_path/pending/ for IN_MOVED_TO — Crashpad writes minidumps to
  // new/<uuid>.dmp then atomically renames to pending/<uuid>.dmp.  The file is
  // fully written and ready to read only after the rename completes.
  const int watch_fd = inotify_add_watch(ifd, pending_dir.c_str(), IN_MOVED_TO);
  if (watch_fd < 0) {
    perror("inotify_add_watch");
    (void)close(ifd);
    (void)close(listen_fd);
    return 1;
  }

  Log(std::string{"crashomon-watcherd: watching "} + pending_dir);

  // Buffer sized for 16 events with maximum-length names.
  constexpr size_t buf_size = sizeof(struct inotify_event) + NAME_MAX + 1;
  std::array<char, buf_size * 16> buf{};

  // Poll on both inotify fd and the listen socket.
  std::array<struct pollfd, 2> pfds{};
  pfds[0].fd = ifd;
  pfds[0].events = POLLIN;
  pfds[1].fd = listen_fd;
  pfds[1].events = POLLIN;

  while (g_stop == 0) {
    const int ready = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), /*timeout_ms=*/500);
    if (ready < 0) {
      if (errno == EINTR) { continue; }
      perror("poll");
      break;
    }
    if (ready == 0) { continue; }

    // ── New client connection ─────────────────────────────────────────────
    if ((pfds[1].revents & POLLIN) != 0) {
      AcceptAndHandleClient(listen_fd, &crash_handler);
    }

    // ── New minidump written ──────────────────────────────────────────────
    if ((pfds[0].revents & POLLIN) == 0) { continue; }

    const ssize_t num_read = read(ifd, buf.data(), buf.size());
    if (num_read < 0) {
      if (errno == EINTR || errno == EAGAIN) { continue; }
      perror("read(inotify)");
      break;
    }

    ProcessInotifyEvents(pending_dir, prune_cfg, buf.data(), num_read);
  }

  (void)inotify_rm_watch(ifd, watch_fd);
  (void)close(ifd);
  (void)close(listen_fd);
  (void)unlink(socket_path.c_str());
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  const char* db_path_env = getenv("CRASHOMON_DB_PATH");
  std::string db_path = (db_path_env != nullptr) ? db_path_env : "/var/crashomon";

  const char* socket_path_env = getenv("CRASHOMON_SOCKET_PATH");
  std::string socket_path =
      (socket_path_env != nullptr) ? socket_path_env : "/run/crashomon/handler.sock";

  uint64_t max_bytes = 0;
  uint32_t max_age_sec = 0;

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
    } else {
      Log(std::string{"crashomon-watcherd: unknown argument: "} + arg);
      return 1;
    }
  }

  struct sigaction sig_action{};
  sig_action.sa_handler = HandleSignal;
  (void)sigemptyset(&sig_action.sa_mask);
  (void)sigaction(SIGTERM, &sig_action, nullptr);
  (void)sigaction(SIGINT, &sig_action, nullptr);

  crashomon::DiskManagerConfig prune_cfg;
  prune_cfg.db_path = db_path;
  prune_cfg.max_bytes = max_bytes;
  prune_cfg.max_age_seconds = max_age_sec;

  return RunWatcher(db_path, socket_path, prune_cfg);
}
