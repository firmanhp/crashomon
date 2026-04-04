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

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/files/scoped_file.h"
#include "client/crash_report_database.h"
#include "daemon/disk_manager.h"
#include "tombstone/minidump_reader.h"
#include "tombstone/tombstone_formatter.h"
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
// Suppress identical crash signatures within this window to limit log/disk spam.
constexpr std::chrono::seconds kRateLimitWindow{30};
// Size of the hex buffer for a uint64_t address (16 hex digits + null terminator).
constexpr size_t kFaultAddrHexBufSize = 17;
// Radix for fault address formatting.
constexpr int kHexBase = 16;
#ifdef HAVE_LIBSYSTEMD
// Kick the systemd watchdog at this interval (must be < WatchdogSec / 2).
constexpr std::chrono::seconds kWatchdogKickInterval{5};
#endif

// POSIX signal handlers can
// only communicate via volatile sig_atomic_t globals; no async-signal-safe alternative.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t g_stop = 0;

void HandleSignal(int /*sig*/) { g_stop = 1; }

// ── Logging ───────────────────────────────────────────────────────────────────

void Log(std::string_view msg) {
  std::fwrite(msg.data(), 1, msg.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

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
  // argv/C-string pointer
  // arithmetic; no safe alternative without a third-party span library.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  if (arg[key_len] == '=') {
    return arg + key_len + 1;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
  return nullptr;
}

// ── Worker state ──────────────────────────────────────────────────────────────

// Shared state for the minidump processing worker thread.
// `pending`, `cv`, and `stop` are shared between the poll loop and the worker,
// protected by `mu`.  `rate_limit_map` is accessed only from the worker thread.
struct WorkerState {
  std::queue<std::string> pending;
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
  // Keyed by "process_name:signal_info:fault_addr_hex"; value is last-seen time.
  // Single-threaded access (worker only) — no additional lock needed.
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> rate_limit_map;
};

// ── Minidump processing ───────────────────────────────────────────────────────

void ProcessNewMinidump(const std::string& path, WorkerState& state,
                        const crashomon::DiskManagerConfig& prune_cfg) {
  auto info_or = crashomon::ReadMinidump(path);
  if (!info_or.ok()) {
    Log(std::string{"crashomon-watcherd: failed to read minidump '"} + path +
        "': " + std::string(info_or.status().message()));
    return;
  }
  const auto& info = *info_or;

  // Rate limiting: suppress identical crash signatures within kRateLimitWindow.
  // Build the key using to_chars (non-variadic, no printf dependency).
  std::array<char, kFaultAddrHexBufSize> fault_hex{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  std::to_chars(fault_hex.data(), fault_hex.data() + fault_hex.size() - 1,
                info.fault_addr, kHexBase);
  const std::string key =
      info.process_name + ":" + info.signal_info + ":" + fault_hex.data();

  const auto now = std::chrono::steady_clock::now();
  const auto rate_it = state.rate_limit_map.find(key);
  if (rate_it != state.rate_limit_map.end() && (now - rate_it->second) < kRateLimitWindow) {
    Log(std::string{"crashomon-watcherd: suppressed duplicate crash ("} + key + ")");
    // Still prune — the minidump file was already written to disk.
    if (const auto prune_status = crashomon::PruneMinidumps(prune_cfg); !prune_status.ok()) {
      Log(std::string{"crashomon-watcherd: prune failed: "} +
          std::string(prune_status.message()));
    }
    return;
  }
  state.rate_limit_map[key] = now;

  Log(crashomon::FormatTombstone(info));
  if (const auto prune_status = crashomon::PruneMinidumps(prune_cfg); !prune_status.ok()) {
    Log(std::string{"crashomon-watcherd: prune failed: "} +
        std::string(prune_status.message()));
  }
}

// ── Worker thread ─────────────────────────────────────────────────────────────

// Dequeues and processes minidumps until signalled to stop.  Drains the queue
// before returning so that minidumps written before shutdown are not lost.
void RunWorker(WorkerState& state, const crashomon::DiskManagerConfig& prune_cfg) {
  while (true) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(state.mu);
      state.cv.wait(lock, [&state] {
        return state.stop || !state.pending.empty();
      });
      if (state.stop && state.pending.empty()) {
        return;
      }
      path = std::move(state.pending.front());
      state.pending.pop();
    }
    // No lock held during processing — allows the poll loop to keep enqueuing.
    ProcessNewMinidump(path, state, prune_cfg);
  }
}

// ── Crash handler hosting ─────────────────────────────────────────────────────

// Create, bind, and listen on a SOCK_SEQPACKET Unix domain socket.
// Refuses to start if a live daemon is already bound to socket_path.
// Returns the listening fd on success, or -1 on failure.
int CreateListenSocket(const std::string& socket_path) {
  base::ScopedFD sock_fd_scoped(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  if (!sock_fd_scoped.is_valid()) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  // Fill sun_path; explicitly null-terminate to handle paths that exactly fill the buffer.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

  // Probe: if a live daemon already owns the socket, refuse to steal it.
  {
    base::ScopedFD probe(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (probe.is_valid()) {
      // POSIX connect requires casting sockaddr_un* to sockaddr*; no standard-compliant alternative.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      if (connect(probe.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) ==
          0) {
        Log(std::string{"crashomon-watcherd: socket "} + socket_path +
            " is held by a live daemon — refusing to start");
        return -1;
      }
      // ENOENT / ECONNREFUSED / ENOTSOCK → stale socket, safe to unlink.
    }
  }

  // Remove stale socket from a previous run.
  unlink(socket_path.c_str());

  // POSIX bind/connect require
  // casting sockaddr_un* to sockaddr*; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (bind(sock_fd_scoped.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("bind");
    return -1;
  }

  // Allow all users to connect (monitored processes may run as different users).
  chmod(socket_path.c_str(), kSocketPermissions);

  if (listen(sock_fd_scoped.get(), /*backlog=*/kListenBacklog) != 0) {
    perror("listen");
    return -1;
  }

  return sock_fd_scoped.release();
}

// Accept one registration connection and send the shared Crashpad client fd +
// daemon PID to the registering process via SCM_RIGHTS, then close the
// accepted fd.  Non-blocking: a single sendmsg call.
// listen_fd (incoming connections) and shared_client_fd (pre-created Crashpad
// socket) are both int but serve different roles; pid_t distinguishes the third.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void AcceptAndShareSocket(int listen_fd, int shared_client_fd, pid_t handler_pid) {
  const int conn_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (conn_fd < 0) {
    if (errno != EINTR && errno != EAGAIN) {
      perror("accept4");
    }
    return;
  }

  // iovec payload: handler PID so the client can call prctl(PR_SET_PTRACER).
  struct iovec iov {};
  iov.iov_base = &handler_pid;
  iov.iov_len = sizeof(handler_pid);

  // cmsg: pass shared_client_fd via SCM_RIGHTS (kernel dups the fd for the
  // recipient process).
  std::array<char, CMSG_SPACE(sizeof(int))> cmsg_buf{};

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf.data();
  msg.msg_controllen = cmsg_buf.size();

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

  close(conn_fd);
}

// ── inotify event processing ──────────────────────────────────────────────────

// Process a raw inotify read buffer, enqueuing full paths for each
// IN_MOVED_TO event on a .dmp file.
void EnqueueInotifyEvents(std::string_view pending_dir, WorkerState& worker_state,
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
    const size_t name_len = strnlen(name, event->len);
    // suffix check on C string;
    // string_view::ends_with would require copying name into string first.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (name_len < 4 || strcmp(name + name_len - 4, ".dmp") != 0) {
      continue;
    }

    const std::string path =
        std::string(pending_dir) + "/" + std::string(name, name_len);
    {
      std::lock_guard<std::mutex> lock(worker_state.mu);
      worker_state.pending.push(path);
    }
    worker_state.cv.notify_one();
  }
}

// ── inotify + accept loop ─────────────────────────────────────────────────────

// Complexity comes from a single sequential setup + event loop that must handle
// Crashpad init, inotify, socket accept, worker thread coordination, and clean
// shutdown in one place.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
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
    mkdir(new_dir.c_str(), kDirPermissions);
    mkdir(pending_dir.c_str(), kDirPermissions);
  }

  auto database = crashpad::CrashReportDatabase::Initialize(base::FilePath(db_path));
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

  base::ScopedFD listen_fd_scoped(CreateListenSocket(socket_path));
  if (!listen_fd_scoped.is_valid()) {
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
  std::array<int, 2> sock_pair{};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sock_pair.data()) != 0) {
    perror("socketpair");
    return 1;
  }
  const int server_fd = sock_pair[0];
  const int client_fd = sock_pair[1];

  // Enable credential passing on both ends so ExceptionHandlerServer can read
  // each client's PID via SCM_CREDENTIALS and ptrace-attach correctly.  Socket
  // options are per-socket-object, so SCM_RIGHTS recipients inherit this.
  int one = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
  setsockopt(client_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

  const pid_t handler_pid = getpid();

  // ── inotify setup ─────────────────────────────────────────────────────────

  base::ScopedFD ifd_scoped(inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
  if (!ifd_scoped.is_valid()) {
    perror("inotify_init1");
    return 1;
  }

  // Watch db_path/pending/ for IN_MOVED_TO — Crashpad writes minidumps to
  // new/<uuid>.dmp then atomically renames to pending/<uuid>.dmp.  The file is
  // fully written and ready to read only after the rename completes.
  const int watch_fd = inotify_add_watch(ifd_scoped.get(), pending_dir.c_str(), IN_MOVED_TO);
  if (watch_fd < 0) {
    perror("inotify_add_watch");
    return 1;
  }

  Log(std::string{"crashomon-watcherd: watching "} + pending_dir);

  // All setup complete — notify systemd that we are ready.
#ifdef HAVE_LIBSYSTEMD
  sd_notify(0, "READY=1");
#endif

  // ── Spawn threads ─────────────────────────────────────────────────────────

  // Single handler thread — ExceptionHandlerServer multiplexes all clients.
  std::thread handler_thread([server_fd, &crash_handler]() {
    crashpad::ExceptionHandlerServer server;
    if (server.InitializeWithClient(base::ScopedFD(server_fd),
                                    /*multiple_clients=*/true)) {
      server.Run(&crash_handler);
    }
  });
  // Keep a reference to Stop() the server on shutdown.
  // The ExceptionHandlerServer* is only accessible from inside the thread, so
  // we use close(client_fd) to make Run() exit: it returns when all client-end
  // holders have closed their copies.

  WorkerState worker_state;
  std::thread worker_thread(RunWorker, std::ref(worker_state), std::cref(prune_cfg));

  // Buffer sized for kInotifyEventBufferCount events with maximum-length names.
  constexpr size_t buf_size = sizeof(struct inotify_event) + NAME_MAX + 1;
  std::array<char, buf_size * kInotifyEventBufferCount> buf{};

  // Poll on both inotify fd and the listen socket.
  std::array<struct pollfd, 2> pfds{};
  pfds[0].fd = ifd_scoped.get();
  pfds[0].events = POLLIN;
  pfds[1].fd = listen_fd_scoped.get();
  pfds[1].events = POLLIN;

#ifdef HAVE_LIBSYSTEMD
  auto last_watchdog = std::chrono::steady_clock::now();
#endif

  while (g_stop == 0) {
    const int ready = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), /*timeout_ms=*/500);

#ifdef HAVE_LIBSYSTEMD
    {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_watchdog >= kWatchdogKickInterval) {
        sd_notify(0, "WATCHDOG=1");
        last_watchdog = now;
      }
    }
#endif

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
      AcceptAndShareSocket(listen_fd_scoped.get(), client_fd, handler_pid);
    }

    // ── New minidump written ──────────────────────────────────────────────
    if ((pfds[0].revents & POLLIN) == 0) {
      continue;
    }

    const ssize_t num_read = read(ifd_scoped.get(), buf.data(), buf.size());
    if (num_read < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      perror("read(inotify)");
      break;
    }

    EnqueueInotifyEvents(pending_dir, worker_state,
                         std::span<const char>{buf.data(), static_cast<size_t>(num_read)});
  }

  // ── Shutdown ──────────────────────────────────────────────────────────────
  // Close the daemon's copy of the client fd.  Once all SCM_RIGHTS recipients
  // also close theirs, ExceptionHandlerServer::Run() returns naturally.
  close(client_fd);
  if (handler_thread.joinable()) {
    handler_thread.join();
  }

  // Signal the worker to drain its queue and exit.  Join after handler_thread
  // so that any final minidumps written by Crashpad are enqueued before we stop.
  {
    std::lock_guard<std::mutex> lock(worker_state.mu);
    worker_state.stop = true;
  }
  worker_state.cv.notify_one();
  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  inotify_rm_watch(ifd_scoped.get(), watch_fd);
  // ifd_scoped and listen_fd_scoped auto-close on scope exit.
  unlink(socket_path.c_str());
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

  struct sigaction sig_action {};
  sig_action.sa_handler = HandleSignal;
  sigemptyset(&sig_action.sa_mask);
  sigaction(SIGTERM, &sig_action, nullptr);
  sigaction(SIGINT, &sig_action, nullptr);

  crashomon::DiskManagerConfig prune_cfg;
  prune_cfg.db_path = db_path;
  prune_cfg.max_bytes = max_bytes;
  prune_cfg.max_age_seconds = max_age_sec;

  return RunWatcher(db_path, socket_path, prune_cfg);
}
