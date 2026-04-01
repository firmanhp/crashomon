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

#include <linux/limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "client/crash_report_database.h"
#include "daemon/disk_manager.h"
#include "daemon/minidump_reader.h"
#include "daemon/tombstone_formatter.h"
#include "handler/linux/crash_report_exception_handler.h"
#include "handler/linux/exception_handler_server.h"

namespace {

// ── Named constants ───────────────────────────────────────────────────────────

constexpr uint64_t kBytesPerKilobyte = 1024ULL;
constexpr uint32_t kSecondsPerHour = 3600U;
constexpr uint32_t kSecondsPerDay = 86400U;
constexpr mode_t kSocketPermissions = 0666;
constexpr int kListenBacklog = 128;
constexpr mode_t kDirPermissions = 0755;
constexpr size_t kInotifyEventBufferCount = 16;

// POSIX signal handlers can
// only communicate via volatile sig_atomic_t globals; no async-signal-safe alternative.
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
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
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
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
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
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
const char* GetArgValue(const char* arg, const char* key) {
  const size_t key_len = strlen(key);
  if (strncmp(arg, key, key_len) != 0) {
    return nullptr;
  }
  // argv/C-string pointer
  // arithmetic; no safe alternative without a third-party span library.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (arg[key_len] == '=') {
    return arg + key_len + 1;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return nullptr;
}

// ── Minidump processing ───────────────────────────────────────────────────────
// TODO: Breakpad MinidumpProcessor crashes the daemon under high crash load.
// ProcessNewMinidump is temporarily disabled pending investigation and fix.
// See: test/stress_test.sh failure with ProcessNewMinidump enabled.

// ── Crash handler hosting ─────────────────────────────────────────────────────

// Create, bind, and listen on a SOCK_SEQPACKET Unix domain socket.
// Returns the listening fd on success, or -1 on failure.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int CreateListenSocket(const std::string& socket_path) {
  const int sock_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock_fd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  // sun_path is a POSIX
  // fixed-size C array; strncpy is the prescribed way to fill it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  // Remove stale socket from a previous run.
  (void)unlink(socket_path.c_str());

  // POSIX bind/connect require
  // casting sockaddr_un* to sockaddr*; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (bind(sock_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("bind");
    (void)close(sock_fd);
    return -1;
  }

  // Allow all users to connect (monitored processes may run as different users).
  (void)chmod(socket_path.c_str(), kSocketPermissions);

  if (listen(sock_fd, /*backlog=*/kListenBacklog) != 0) {
    perror("listen");
    (void)close(sock_fd);
    return -1;
  }

  return sock_fd;
}

// Accept one registration connection and send the shared Crashpad client fd +
// daemon PID to the registering process via SCM_RIGHTS, then close the
// accepted fd.  Non-blocking: a single sendmsg call.
void AcceptAndShareSocket(int listen_fd, int shared_client_fd, pid_t handler_pid) {
  const int conn_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (conn_fd < 0) {
    if (errno != EINTR && errno != EAGAIN) {
      perror("accept4");
    }
    return;
  }

  // iovec payload: handler PID so the client can call prctl(PR_SET_PTRACER).
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  struct iovec iov {};
  iov.iov_base = &handler_pid;
  iov.iov_len = sizeof(handler_pid);

  // cmsg: pass shared_client_fd via SCM_RIGHTS (kernel dups the fd for the
  // recipient process).
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
  char cmsg_buf[CMSG_SPACE(sizeof(int))]{};

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  // CMSG macros use pointer casts
  // internally; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &shared_client_fd, sizeof(int));

  // MSG_NOSIGNAL prevents SIGPIPE if the client disconnected before receiving
  // (e.g. crashed immediately after connect).
  if (sendmsg(conn_fd, &msg, MSG_NOSIGNAL) < 0) {
    if (errno != EPIPE) {
      perror("sendmsg(SCM_RIGHTS)");
    }
  }

  (void)close(conn_fd);
}

// ── inotify event processing ──────────────────────────────────────────────────

// Process a raw inotify read buffer, triggering minidump handling for each
// IN_MOVED_TO event on a .dmp file.
void ProcessInotifyEvents(std::string_view /*pending_dir*/,
                          const crashomon::DiskManagerConfig& /*prune_cfg*/,
                          std::span<const char> buf) {
  for (size_t offset = 0; offset < buf.size();) {
    // inotify_event is a
    // variable-length C kernel struct; reinterpret_cast of a byte buffer is the only way to access
    // it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* event = reinterpret_cast<const struct inotify_event*>(buf.data() + offset);
    offset += sizeof(struct inotify_event) + event->len;

    if ((event->mask & IN_MOVED_TO) == 0) {
      continue;
    }
    if (event->len == 0) {
      continue;
    }

    // event->name is a C
    // flexible array member; decaying to pointer is the only access method.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* name = event->name;
    const size_t name_len =
        strnlen(name, event->len);  // NOLINT(misc-include-cleaner) — strnlen is in <cstring> which
                                    // is included; false positive from include-cleaner.
    // suffix check on C string;
    // string_view::ends_with would require copying name into string first.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (name_len < 4 || strcmp(name + name_len - 4, ".dmp") != 0) {
      continue;
    }

    // TODO: call ProcessNewMinidump here once Breakpad crash is fixed.
  }
}

// ── inotify + accept loop ─────────────────────────────────────────────────────

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int RunWatcher(const std::string& db_path, const std::string& socket_path,
               const crashomon::DiskManagerConfig& prune_cfg) {
  // ── Crashpad handler setup ────────────────────────────────────────────────

  // Crashpad database lifecycle: writes to new/, then renames to pending/.
  // We watch pending/ for IN_MOVED_TO — the file is fully written and renamed
  // at that point. new/ is created by CrashReportDatabase::Initialize().
  const std::string new_dir = db_path + "/new";
  const std::string pending_dir = db_path + "/pending";
  {
    struct stat stat_buf {};
    if (::stat(db_path.c_str(), &stat_buf) != 0 || !S_ISDIR(stat_buf.st_mode)) {
      Log(std::string{"crashomon-watcherd: db-path is not a directory: "} + db_path);
      return 1;
    }
    // mkdir is best-effort; dirs may already exist after CrashReportDatabase::Initialize().
    (void)mkdir(new_dir.c_str(), kDirPermissions);
    (void)mkdir(pending_dir.c_str(), kDirPermissions);
  }

  auto database = crashpad::CrashReportDatabase::Initialize(
      base::FilePath(db_path));  // NOLINT(misc-include-cleaner) — base::FilePath comes from
                                 // Crashpad transitively; cannot control third-party include paths.
  if (database == nullptr) {
    Log(std::string{"crashomon-watcherd: failed to initialize crash database at "} + db_path);
    return 1;
  }

  // crash data never leaves the device — no upload thread, no user stream sources.
  // process_annotations and attachments must be non-null (Crashpad dereferences them).
  static const std::map<std::string, std::string> kNoAnnotations;
  static const std::vector<base::FilePath> kNoAttachments;
  crashpad::CrashReportExceptionHandler crash_handler(database.get(),
                                                      /*upload_thread=*/nullptr, &kNoAnnotations,
                                                      &kNoAttachments,
                                                      /*write_minidump_to_database=*/true,
                                                      /*write_minidump_to_log=*/false,
                                                      /*user_stream_data_sources=*/nullptr);

  const int listen_fd = CreateListenSocket(socket_path);
  if (listen_fd < 0) {
    return 1;
  }
  Log(std::string{"crashomon-watcherd: listening on "} + socket_path);

  // ── Shared Crashpad socket ────────────────────────────────────────────────
  //
  // Architecture: a single ExceptionHandlerServer runs on a dedicated thread,
  // owning the server end of a SOCK_SEQPACKET socketpair.  The client end is
  // shared with connecting processes via SCM_RIGHTS fd passing over the listen
  // socket.  Crashpad multiplexes all crash requests on one epoll thread.
  //
  // The daemon keeps a dup of the client end alive so that Run() does not exit
  // prematurely (it exits when all client-end holders close their copies).
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) != 0) {
    perror("socketpair");
    (void)close(listen_fd);
    return 1;
  }
  const int server_fd = sp[0];
  const int client_fd = sp[1];

  // Enable credential passing on both ends so ExceptionHandlerServer can read
  // each client's PID via SCM_CREDENTIALS and ptrace-attach correctly.  Socket
  // options are per-socket-object, so SCM_RIGHTS recipients inherit this.
  int one = 1;
  // SOL_SOCKET/SO_PASSCRED come from <sys/socket.h> which is included; include-cleaner FP.
  // NOLINTNEXTLINE(misc-include-cleaner)
  (void)setsockopt(server_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
  // NOLINTNEXTLINE(misc-include-cleaner)
  (void)setsockopt(client_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

  const pid_t handler_pid = getpid();

  // Single handler thread — ExceptionHandlerServer multiplexes all clients.
  std::thread handler_thread([server_fd, &crash_handler]() {
    crashpad::ExceptionHandlerServer server;
    if (server.InitializeWithClient(
            base::ScopedFD(server_fd),  // NOLINT(misc-include-cleaner)
            /*multiple_clients=*/true)) {
      server.Run(&crash_handler);
    }
  });
  // Keep a reference to Stop() the server on shutdown.
  // The ExceptionHandlerServer* is only accessible from inside the thread, so
  // we use close(client_fd) to make Run() exit: it returns when all client-end
  // holders have closed their copies.

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

  // Buffer sized for kInotifyEventBufferCount events with maximum-length names.
  constexpr size_t buf_size = sizeof(struct inotify_event) + NAME_MAX + 1;
  std::array<char, buf_size * kInotifyEventBufferCount> buf{};

  // Poll on both inotify fd and the listen socket.
  // pollfd/POLLIN/poll/nfds_t all come from <poll.h> which is included; include-cleaner FPs.
  // NOLINTNEXTLINE(misc-include-cleaner)
  std::array<struct pollfd, 2> pfds{};
  pfds[0].fd = ifd;
  pfds[0].events = POLLIN;  // NOLINT(misc-include-cleaner)
  pfds[1].fd = listen_fd;
  pfds[1].events = POLLIN;  // NOLINT(misc-include-cleaner)

  while (g_stop == 0) {
    // poll/nfds_t come from <poll.h> which is included; include-cleaner FPs.
    // NOLINTNEXTLINE(misc-include-cleaner)
    const int ready = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), /*timeout_ms=*/500);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("poll");
      break;
    }
    if (ready == 0) {
      continue;
    }

    // ── New client connection ─────────────────────────────────────────────
    if ((pfds[1].revents & POLLIN) != 0) {
      AcceptAndShareSocket(listen_fd, client_fd, handler_pid);
    }

    // ── New minidump written ──────────────────────────────────────────────
    if ((pfds[0].revents & POLLIN) == 0) {
      continue;
    }

    const ssize_t num_read = read(ifd, buf.data(), buf.size());
    if (num_read < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      perror("read(inotify)");
      break;
    }

    ProcessInotifyEvents(pending_dir, prune_cfg,
                         std::span<const char>{buf.data(), static_cast<size_t>(num_read)});
  }

  // ── Shutdown ──────────────────────────────────────────────────────────────
  // Close the daemon's copy of the client fd.  Once all SCM_RIGHTS recipients
  // also close theirs, ExceptionHandlerServer::Run() returns naturally.
  (void)close(client_fd);
  if (handler_thread.joinable()) {
    handler_thread.join();
  }

  (void)inotify_rm_watch(ifd, watch_fd);
  (void)close(ifd);
  (void)close(listen_fd);
  (void)unlink(socket_path.c_str());
  return 0;
}

}  // namespace

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int main(int argc, char* argv[]) {
  const char* db_path_env = getenv("CRASHOMON_DB_PATH");
  std::string db_path = (db_path_env != nullptr) ? db_path_env : "/var/crashomon";

  const char* socket_path_env = getenv("CRASHOMON_SOCKET_PATH");
  std::string socket_path =
      (socket_path_env != nullptr) ? socket_path_env : "/run/crashomon/handler.sock";

  uint64_t max_bytes = 0;
  uint32_t max_age_sec = 0;

  // argv is a C array; pointer
  // arithmetic is the only way to construct a range from it.
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

  // sigaction/sa_handler/sigemptyset from <csignal>; include-cleaner FPs.
  // NOLINTNEXTLINE(misc-include-cleaner)
  struct sigaction sig_action {};
  sig_action.sa_handler = HandleSignal;            // NOLINT(misc-include-cleaner)
  (void)sigemptyset(&sig_action.sa_mask);          // NOLINT(misc-include-cleaner)
  (void)sigaction(SIGTERM, &sig_action, nullptr);  // NOLINT(misc-include-cleaner)
  (void)sigaction(SIGINT, &sig_action, nullptr);   // NOLINT(misc-include-cleaner)

  crashomon::DiskManagerConfig prune_cfg;
  prune_cfg.db_path = db_path;
  prune_cfg.max_bytes = max_bytes;
  prune_cfg.max_age_seconds = max_age_sec;

  return RunWatcher(db_path, socket_path, prune_cfg);
}
