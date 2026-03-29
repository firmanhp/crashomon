# Plan: Watcherd as Crashpad Handler — IMPLEMENTED 2026-03-30

## Context

`libcrashomon.so` calls `CrashpadClient::StartHandler()` which `posix_spawn`s a `crashpad_handler`
subprocess. That subprocess inherits `LD_PRELOAD=libcrashomon.so`, which re-runs the library
constructor, which spawns another handler — infinite fork bomb. The system exhausts process
slots within milliseconds.

This is unfixable within the current architecture without fragile hacks (detecting own binary
via `/proc/self/exe`, unsetting `LD_PRELOAD` before spawn, etc.). The root cause is that crash
handling requires spawning a subprocess, and `LD_PRELOAD` inheritance is by design.

The fix: `crashomon-watcherd` (already a long-running systemd daemon) becomes the Crashpad
crash handler directly. Client processes `connect()` to it via a Unix domain socket — no
subprocess spawning, no `LD_PRELOAD` inheritance problem.

**Scope**: `daemon/main.cpp`, `lib/crashomon.cpp`, `lib/crashomon_internal.h`, `lib/crashomon.h`,
`daemon/CMakeLists.txt`, systemd units, tests. The daemon modules (`minidump_reader`,
`tombstone_formatter`, `disk_manager`), web UI, and analysis tools are unaffected.

---

## Approach

**Per-client accept model** — the watcherd binds a `SOCK_SEQPACKET` Unix domain socket,
`accept()`s connections from client processes, and spawns a per-client handler thread.

```
[App with LD_PRELOAD=libcrashomon.so]
  │  constructor: connect(SOCK_SEQPACKET) → /run/crashomon/handler.sock
  │  SetHandlerSocket(fd, -1)  →  discovers watcherd PID via SCM_CREDENTIALS
  │  prctl(PR_SET_PTRACER, watcherd_pid)
  │
  │  <crash>  →  sendmsg(kTypeCrashDumpRequest) on per-client socket
  │           →  sigtimedwait(SIGCONT, 5s)
  │
  v
[crashomon-watcherd]  (single systemd service, replaces crashpad_handler)
  │  accept() → per-client thread with ExceptionHandlerServer
  │  ptrace-attach → capture minidump → write to CrashReportDatabase → SIGCONT
  │  inotify: detect new .dmp → ReadMinidump → FormatTombstone → stderr/journald
  │  PruneMinidumps (existing)
```

Why this design:

- **`SetHandlerSocket()` hardcodes `multiple_clients_=true`** in the crash signal handler.
  Server side MUST use `InitializeWithClient(fd, /*multiple_clients=*/true)` to match.
  Crash completion is via SIGCONT signal, not socket reply.

- **Per-client sockets are race-free.** Each `accept()`ed fd is a private connection.
  `GetHandlerCredentials()` (used to discover watcherd PID) sends/receives on a private
  socket — no cross-client confusion even during concurrent initialization.

- **Thread auto-cleanup.** `ExceptionHandlerServer::Run()` exits when `clients_.size() == 0`,
  which happens when the client process closes its socket (exit or crash). The detached
  thread returns and is cleaned up.

- **`CrashReportExceptionHandler::HandleException()` is thread-safe.** The shared
  `CrashReportDatabase` uses an internal mutex. All other state (`DirectPtraceConnection`,
  `ProcessSnapshotLinux`, `MinidumpFileWriter`) is on-stack per call.

- **`ptrace_scope=1` works.** Client calls `prctl(PR_SET_PTRACER, watcherd_pid)` at init
  time. With `multiple_clients=true`, the strategy decider returns `kDirectPtrace` immediately
  (no back-and-forth with the client at crash time).

---

## Modified files

### 1. `lib/crashomon_internal.h` — Config resolution rename

Replace handler path with socket path throughout:

```cpp
constexpr std::string_view kDefaultSocketPath = "/run/crashomon/handler.sock";

struct ResolvedConfig {
  std::string db_path;       // unused by client, kept for struct compat
  std::string socket_path;   // was handler_path
};
```

`Resolve()`: env var precedence becomes `CRASHOMON_SOCKET_PATH` (was `CRASHOMON_HANDLER_PATH`).
Default changes from `/usr/libexec/crashomon/crashpad_handler` to `/run/crashomon/handler.sock`.

### 2. `lib/crashomon.h` — Public C API update

```c
typedef struct CrashomonConfig {
  const char *db_path;       /* Minidump database path (NULL = use env/default) */
  const char *socket_path;   /* watcherd socket path   (NULL = use env/default) */
} CrashomonConfig;
```

Update doc comments: remove `crashpad_handler` references, document `CRASHOMON_SOCKET_PATH`.

### 3. `lib/crashomon.cpp` — Rewrite `DoInit()`

Replace `CrashpadClient::StartHandler()` with socket connect + `SetHandlerSocket()`:

```cpp
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "client/crashpad_client.h"

int DoInit(const ResolvedConfig& cfg) {
  int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock < 0) return -1;

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, cfg.socket_path.c_str(), sizeof(addr.sun_path) - 1);

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return -1;  // watcherd not running — silent degradation, same as StartHandler failure
  }

  int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

  static crashpad::CrashpadClient client;
  // pid=-1: GetHandlerCredentials() discovers watcherd PID via SCM_CREDENTIALS on the
  //         per-client socket, then prctl(PR_SET_PTRACER, watcherd_pid), then installs
  //         crash signal handlers.
  return client.SetHandlerSocket(base::ScopedFD(sock), /*pid=*/-1) ? 0 : -1;
}
```

Remove `#include "client/crashpad_client.h"` preamble that pulled in `StartHandler` deps.
The `crashpad_client` link dep stays — `SetHandlerSocket` is part of the same library.

### 4. `daemon/main.cpp` — Add crash handler hosting

Extend the existing inotify `poll()` loop to also handle a listening socket.

**New includes:**
```cpp
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>

#include "client/crash_report_database.h"
#include "handler/linux/crash_report_exception_handler.h"
#include "handler/linux/exception_handler_server.h"
```

**New startup (before main loop):**
```cpp
// 1. Initialize Crashpad database at db_path
auto database = crashpad::CrashReportDatabase::Initialize(base::FilePath(db_path));

// 2. Create crash report exception handler (the Delegate)
std::map<std::string, std::string> annotations;
std::vector<base::FilePath> attachments;
crashpad::CrashReportExceptionHandler crash_handler(
    database.get(),
    /*upload_thread=*/nullptr,    // no uploads (url="")
    &annotations,
    &attachments,
    /*write_minidump_to_database=*/true,
    /*write_minidump_to_log=*/false,
    /*user_stream_data_sources=*/nullptr);

// 3. Create + bind + listen SOCK_SEQPACKET socket
int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
struct sockaddr_un addr{};
addr.sun_family = AF_UNIX;
strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
unlink(socket_path.c_str());  // remove stale socket from prior run
bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
chmod(socket_path.c_str(), 0666);  // allow non-root clients to connect
listen(listen_fd, SOMAXCONN);
```

**Extended poll loop (2 fds instead of 1):**
```cpp
struct pollfd pfds[2];
pfds[0] = {.fd = inotify_fd, .events = POLLIN};
pfds[1] = {.fd = listen_fd,  .events = POLLIN};

while (!g_stop) {
  int ready = poll(pfds, 2, 500);
  if (ready < 0) { if (errno == EINTR) continue; break; }
  if (ready == 0) continue;

  // Handle new client connections
  if (pfds[1].revents & POLLIN) {
    int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (client_fd >= 0) {
      int one = 1;
      setsockopt(client_fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
      // Per-client handler thread
      std::thread([client_fd, &crash_handler]() {
        crashpad::ExceptionHandlerServer server;
        if (server.InitializeWithClient(base::ScopedFD(client_fd), true)) {
          server.Run(&crash_handler);
        }
      }).detach();
    }
  }

  // Handle inotify events (existing code, unchanged)
  if (pfds[0].revents & POLLIN) { /* ... existing inotify read + ProcessNewMinidump ... */ }
}
```

**New CLI arg:** `--socket-path=PATH` (default: env `CRASHOMON_SOCKET_PATH` or `/run/crashomon/handler.sock`).

**inotify watch path change:** Watch `db_path + "/new"` instead of `db_path`.
Crashpad's `CrashReportDatabase` writes minidumps to `<db_path>/new/<uuid>`. The inotify
filter for `.dmp` extension may need updating if Crashpad uses a different extension (to be
verified during implementation — may use no extension or UUID-only naming).

**Cleanup on shutdown:** `unlink(socket_path)`, `close(listen_fd)`.

### 5. `daemon/CMakeLists.txt` — Add Crashpad link deps

```cmake
target_link_libraries(crashomon_watcherd
  PRIVATE
    crashomon_warnings
    breakpad_processor
    crashpad_handler_lib    # ExceptionHandlerServer, CrashReportExceptionHandler
    crashpad_client         # CrashReportDatabase, CrashpadClient types
    absl::status
    absl::statusor
    Threads::Threads
)
```

Add SYSTEM include directories for Crashpad headers (same pattern as existing Abseil/Breakpad
workarounds):

```cmake
if(TARGET crashpad_interface)
  target_include_directories(crashomon_watcherd SYSTEM PRIVATE
    $<TARGET_PROPERTY:crashpad_interface,INTERFACE_INCLUDE_DIRECTORIES>
  )
endif()
```

### 6. `systemd/example-app.service.d/crashomon.conf`

```ini
[Service]
Environment=LD_PRELOAD=/usr/lib/libcrashomon.so
Environment=CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock
After=crashomon-watcherd.service
Requires=crashomon-watcherd.service
```

Remove `CRASHOMON_HANDLER_PATH` and `CRASHOMON_DB_PATH` (client no longer needs them).
Add `After=` / `Requires=` so watcherd is running before the app starts.

### 7. `systemd/crashomon-watcherd.service` — No changes needed

Already has `RuntimeDirectory=crashomon` (creates `/run/crashomon/`).
Socket path `/run/crashomon/handler.sock` lives there.

### 8. `test/test_crashomon.cpp` — Update config tests

All 17 tests exercise `Resolve()` and `GetEnv()`. Changes:
- `handler_path` → `socket_path` in `ResolvedConfig` field access
- `CRASHOMON_HANDLER_PATH` → `CRASHOMON_SOCKET_PATH` in `ScopedEnv` calls
- `kDefaultHandlerPath` → `kDefaultSocketPath` in expected values
- Default value changes from `/usr/libexec/crashomon/crashpad_handler` to `/run/crashomon/handler.sock`

### 9. `test/gen_fixtures.sh` and `test/integration_test.sh`

Must start `crashomon-watcherd` in background before running crashers:
```bash
DB=$(mktemp -d)
SOCKET="$DB/handler.sock"
./build/daemon/crashomon-watcherd --db-path="$DB" --socket-path="$SOCKET" &
WATCHERD_PID=$!
sleep 0.5  # wait for socket to appear

CRASHOMON_SOCKET_PATH="$SOCKET" LD_PRELOAD=./build/lib/libcrashomon.so \
  ./build/examples/crashomon-example-segfault

kill "$WATCHERD_PID"
```

### 10. `CMakeLists.txt` (root) — Optional cleanup

Keep `crashpad_handler` build target (useful as standalone tool). Consider removing from
default `install()` targets since it's no longer required for normal operation.

---

## Implementation order

1. `lib/crashomon_internal.h` — config field rename
2. `lib/crashomon.h` — public API update
3. `lib/crashomon.cpp` — rewrite DoInit
4. `test/test_crashomon.cpp` — update config tests (keep tests green)
5. `daemon/CMakeLists.txt` — add Crashpad link deps
6. `daemon/main.cpp` — add handler hosting
7. `systemd/` — update drop-in
8. `test/gen_fixtures.sh` + `test/integration_test.sh` — watcherd-first flow
9. Build + unit test + smoke test

---

## Verification

1. **Build**: `cmake -B build && cmake --build build` — clean with `-Werror`
2. **Unit tests**: `ctest --test-dir build` — 75 tests pass
3. **Smoke test** (manual):
   ```bash
   # Terminal 1
   mkdir -p /tmp/ct && ./build/daemon/crashomon-watcherd \
     --db-path=/tmp/ct --socket-path=/tmp/ct/handler.sock

   # Terminal 2
   CRASHOMON_SOCKET_PATH=/tmp/ct/handler.sock \
     LD_PRELOAD=./build/lib/libcrashomon.so \
     ./build/examples/crashomon-example-segfault
   ```
   Expect: tombstone in terminal 1, .dmp in `/tmp/ct/new/`, **no infinite fork loop**.
4. **LD_PRELOAD inheritance**: Run multithread example (it forks). Child processes should
   connect to watcherd independently — no subprocess spawning.
5. **Integration test**: `test/integration_test.sh` (after updates)

---

## Risks / open questions

- **inotify subdirectory + file naming**: Crashpad's `CrashReportDatabase` writes to
  `<db_path>/new/<uuid>`. Need to verify the exact filename pattern — may not use `.dmp`
  extension. The inotify filter may need adjustment.
- **Socket permissions**: `/run/crashomon/handler.sock` created by watcherd. If monitored
  processes run as different UIDs, the socket needs `chmod 0666` or a shared group. The plan
  uses `chmod 0666` which is permissive — tighten if needed.
- **Watcherd not running at client init**: `connect()` fails, `DoInit()` returns -1, process
  runs without crash monitoring. The systemd `Requires=` dependency handles production. For
  dev/test, user must start watcherd first.
- **Thread explosion**: One detached thread per monitored process. On a 4-core embedded
  system monitoring ~20-50 processes, this is fine. Threads are mostly idle (blocked in
  `epoll_wait`). If scale becomes a concern, could move to a single-threaded epoll loop
  with manual client management — but that requires reimplementing `ExceptionHandlerServer`.
- **Crashpad header include paths**: The watcherd now includes Crashpad headers
  (`handler/linux/exception_handler_server.h`, etc.) which pull in mini_chromium's `base/`
  types. Need to ensure SYSTEM include paths suppress all third-party warnings under `-Werror`.
