// daemon/watcher.cpp — inotify + Crashpad event loop

#include "daemon/watcher.h"

#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <array>
#ifdef HAVE_LIBSYSTEMD
#include <chrono>
#endif
#include <csignal>
#include <cstdio>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/files/scoped_file.h"
#include "client/crash_report_database.h"
#include "daemon/crash_socket.h"
#include "daemon/disk_manager.h"
#include "daemon/worker.h"
#include "handler/linux/crash_report_exception_handler.h"
#include "handler/linux/exception_handler_server.h"
#include "spdlog/spdlog.h"

namespace {

constexpr mode_t kDirPermissions = 0755;
constexpr size_t kInotifyEventBufferCount = 16;
#ifdef HAVE_LIBSYSTEMD
// Kick the systemd watchdog at this interval (must be < WatchdogSec / 2).
constexpr std::chrono::seconds kWatchdogKickInterval{5};
#endif

// POSIX signal handlers can only communicate via volatile sig_atomic_t globals;
// no async-signal-safe alternative.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile sig_atomic_t g_stop = 0;

void HandleSignal(int /*sig*/) { g_stop = 1; }

// Process a raw inotify read buffer, enqueuing full paths for each
// IN_MOVED_TO event on a .dmp file.
void EnqueueInotifyEvents(std::string_view pending_dir, crashomon::WorkerState& worker_state,
                          std::span<const char> buf) {
  for (size_t offset = 0; offset < buf.size();) {
    // inotify_event is a variable-length C kernel struct; reinterpret_cast of a
    // byte buffer is the only way to access it.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* event = reinterpret_cast<const struct inotify_event*>(buf.data() + offset);
    offset += sizeof(struct inotify_event) + event->len;

    if ((event->mask & IN_MOVED_TO) == 0) {
      continue;
    }
    if (event->len == 0) {
      continue;
    }

    // event->name is a C flexible array member; decaying to pointer is the only access method.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* name = event->name;
    const size_t name_len = strnlen(name, event->len);
    // suffix check on C string; string_view::ends_with would require copying name into string
    // first. NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (name_len < 4 || strcmp(name + name_len - 4, ".dmp") != 0) {
      continue;
    }

    const std::string path = std::string(pending_dir) + "/" + std::string(name, name_len);
    {
      const std::lock_guard<std::mutex> lock(worker_state.mu);
      worker_state.pending.push(path);
    }
    worker_state.cv.notify_one();
  }
}

}  // namespace

namespace crashomon {

void SetupSignalHandlers() {
  struct sigaction sig_action {};
  sig_action.sa_handler = HandleSignal;
  sigemptyset(&sig_action.sa_mask);
  sigaction(SIGTERM, &sig_action, nullptr);
  sigaction(SIGINT, &sig_action, nullptr);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int RunWatcher(const std::string& db_path, const std::string& socket_path,
               const DiskManagerConfig& prune_cfg, const std::string& export_path) {
  // ── Crashpad handler setup ────────────────────────────────────────────────

  // Crashpad database lifecycle: writes to new/, then renames to pending/.
  // We watch pending/ for IN_MOVED_TO — the file is fully written and renamed
  // at that point. new/ is created by CrashReportDatabase::Initialize().
  const std::string new_dir = db_path + "/new";
  const std::string pending_dir = db_path + "/pending";
  {
    struct stat stat_buf {};
    if (::stat(db_path.c_str(), &stat_buf) != 0 || !S_ISDIR(stat_buf.st_mode)) {
      spdlog::error("crashomon-watcherd: db-path is not a directory: {}", db_path);
      return 1;
    }
    // mkdir is best-effort; dirs may already exist after CrashReportDatabase::Initialize().
    mkdir(new_dir.c_str(), kDirPermissions);
    mkdir(pending_dir.c_str(), kDirPermissions);
  }

  auto database = crashpad::CrashReportDatabase::Initialize(base::FilePath(db_path));
  if (database == nullptr) {
    spdlog::error("crashomon-watcherd: failed to initialize crash database at {}", db_path);
    return 1;
  }

  // Crash data never leaves the device — no upload thread, no user stream sources.
  // process_annotations and attachments must be non-null (Crashpad dereferences them).
  static const std::map<std::string, std::string> kNoAnnotations;
  static const std::vector<base::FilePath> kNoAttachments;
  crashpad::CrashReportExceptionHandler crash_handler(database.get(),
                                                      /*upload_thread=*/nullptr, &kNoAnnotations,
                                                      &kNoAttachments,
                                                      /*write_minidump_to_database=*/true,
                                                      /*write_minidump_to_log=*/false,
                                                      /*user_stream_data_sources=*/nullptr);

  const base::ScopedFD listen_fd_scoped(CreateListenSocket(socket_path));
  if (!listen_fd_scoped.is_valid()) {
    return 1;
  }
  spdlog::info("crashomon-watcherd: listening on {}", socket_path);

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

  const base::ScopedFD ifd_scoped(inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
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

  spdlog::info("crashomon-watcherd: watching {}", pending_dir);

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
  // The ExceptionHandlerServer* is only accessible from inside the thread, so
  // we use close(client_fd) to make Run() exit: it returns when all client-end
  // holders have closed their copies.

  WorkerState worker_state;
  std::thread worker_thread(RunWorker, std::ref(worker_state), std::cref(prune_cfg),
                            std::string_view{export_path});

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
    const std::lock_guard<std::mutex> lock(worker_state.mu);
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

}  // namespace crashomon
