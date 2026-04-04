# Crashomon Assessment Improvements Plan

## Context

Based on `/home/ngiks/crashomon-assessment.md` (dated 2026-04-04), crashomon has a sound core
architecture but needs hardening in three areas: code quality (NOLINT density, strncpy, no RAII),
daemon robustness (silent failure, socket collision, no watchdog, blocking minidump processing, no
rate limiting), and web security (no CSRF, tmp files never cleaned up). This plan addresses all
items except the "missing features" category (remote collection, ARM32, coredump fallback) and
full CI setup, which are out of scope for a correctness-first hardening pass.

Items from the assessment addressed: #1, #2, #3, #4, #5, #6, #8, #9, #14, #15.
Items deferred: #7 (error handling unification — too broad), #10 (CI), #11 (adversarial tests),
#12 (symbolication test), #13 (socket group restriction — documented as intentional), #16–#18
(missing features).

---

## Implementation Steps

### Step 1 — Suppress misc-include-cleaner globally

**File:** `.clang-tidy`

Add `-misc-include-cleaner,` after the `misc-*,` line in the `Checks:` block. With
`WarningsAsErrors: "*"`, all existing `NOLINT(misc-include-cleaner)` comments become dangling
suppressions; remove them in the same step.

**Files with NOLINT(misc-include-cleaner) to scrub:**
- `daemon/main.cpp` — 9 inline NOLINT + 5 NOLINTNEXTLINE comment blocks (plus their explanatory
  prose). Specific locations: strnlen line ~284, base::FilePath line ~323, SO_PASSCRED setsockopt
  lines ~370–373, base::ScopedFD line ~381, pollfd/POLLIN lines ~419–424, poll() line ~428–429,
  sigaction block lines ~514–519.
- `daemon/disk_manager.cpp` — 1 NOLINTNEXTLINE on the `#include <ranges>` line.
- Do NOT remove NOLINTs for other checks (reinterpret-cast, bounds, etc.) — only misc-include-cleaner.

### Step 2 — Replace strncpy with snprintf for sun_path

**Files:** `daemon/main.cpp` (~line 181), `lib/crashomon.cpp` (~line 77)

In both places, replace:
```cpp
// sun_path is a POSIX fixed-size C array; strncpy is the prescribed way to fill it.
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
```
With:
```cpp
// Fill sun_path, truncating to UNIX_PATH_MAX if needed.
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
```
Keep the `cppcoreguidelines-pro-bounds-array-to-pointer-decay` NOLINTNEXTLINE — snprintf still
decays the array to `char*` as its first argument.

### Step 3 — Log on DoInit connection failure

**File:** `lib/crashomon.cpp`

Add `#include <syslog.h>` to the system-include block.

In `DoInit`, the connect-failure path at ~line 82:
```cpp
if (connect(sock, ...) != 0) {
  close(sock);
  return -1;  // watcherd not running — crash monitoring silently disabled
}
```
Change to:
```cpp
if (connect(sock, ...) != 0) {
  syslog(LOG_WARNING, "crashomon: could not connect to watcherd at %s: %s",
         cfg.socket_path.c_str(), strerror(errno));
  close(sock);
  return -1;
}
```
`syslog` is async-signal-safe in the LD_PRELOAD constructor context. No other changes to the
constructor or public API.

### Step 4 — RAII for file descriptors in daemon

**File:** `daemon/main.cpp`

Add `#include "base/files/scoped_file.h"` (available from Crashpad, already linked).

In `RunWatcher`:
- Change `const int listen_fd = CreateListenSocket(...)` → `base::ScopedFD listen_fd_scoped(CreateListenSocket(...))`.  
  Check validity with `listen_fd_scoped.is_valid()`. Pass `listen_fd_scoped.get()` to
  `AcceptAndShareSocket` and to the poll array. Remove all explicit `close(listen_fd)` calls; ScopedFD
  auto-closes on scope exit or `.reset()`.
- Change `const int ifd = inotify_init1(...)` → `base::ScopedFD ifd_scoped(inotify_init1(...))`.  
  Pass `ifd_scoped.get()` to `inotify_add_watch`, `inotify_rm_watch`, and the poll array. Remove
  explicit `close(ifd)` calls.
- `client_fd` (the shared Crashpad socket): keep as raw int. The `close(client_fd)` at shutdown is
  intentional signaling to trigger `ExceptionHandlerServer::Run()` to return — not a cleanup-on-error
  path. Leave it as-is.
- `sock_pair` inside `AcceptAndShareSocket`: also leave as-is; `server_fd` is transferred to
  `base::ScopedFD(server_fd)` by Crashpad's `InitializeWithClient`, and `client_fd` is the return value.

Also apply ScopedFD to `CreateListenSocket`'s internal `sock_fd`:
- Change `const int sock_fd = socket(...)` → `base::ScopedFD sock_fd_scoped(socket(...))`.  
  Each early-return `close(sock_fd); return -1;` becomes just `return -1;` (ScopedFD auto-closes).
  On success, return with `sock_fd_scoped.release()` to transfer ownership to the caller.

### Step 5 — Socket path collision check

**File:** `daemon/main.cpp`, `CreateListenSocket` function

Insert a liveness probe between filling `addr` and the existing `unlink`:

```cpp
// Probe: if a live daemon already owns the socket, refuse to steal it.
{
  base::ScopedFD probe(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  if (probe.is_valid()) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (connect(probe.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
      Log("crashomon-watcherd: socket %s is held by a live daemon — refusing to start",
          socket_path.c_str());
      return -1;  // sock_fd_scoped auto-closes
    }
    // ENOENT / ECONNREFUSED / ENOTSOCK → stale socket, safe to unlink.
  }
}
// Remove stale socket from a previous run.
unlink(socket_path.c_str());
```

The probe ScopedFD goes out of scope immediately after the block. No NOLINT needed for the second
`reinterpret_cast` if the existing pattern uses a comment block — follow the existing style.

### Step 6 — sd_notify watchdog integration

**File:** `daemon/CMakeLists.txt`

After the existing `target_link_libraries` block, add:
```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSYSTEMD libsystemd)
if(LIBSYSTEMD_FOUND)
  target_compile_definitions(crashomon_watcherd PRIVATE HAVE_LIBSYSTEMD=1)
  target_link_libraries(crashomon_watcherd PRIVATE ${LIBSYSTEMD_LIBRARIES})
  target_include_directories(crashomon_watcherd SYSTEM PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})
endif()
```
`SYSTEM PRIVATE` prevents our `-Werror` flags from applying to systemd headers.

**File:** `daemon/main.cpp`

Add conditional include after system headers:
```cpp
#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif
```

In `RunWatcher`, after `inotify_add_watch` succeeds and the existing `Log("watching ...")` line:
```cpp
#ifdef HAVE_LIBSYSTEMD
  sd_notify(0, "READY=1");
#endif
```

In the poll loop, add a watchdog kick timer. Declare before the loop:
```cpp
#ifdef HAVE_LIBSYSTEMD
  auto last_watchdog = std::chrono::steady_clock::now();
#endif
```
Inside the loop after `poll()` returns (including on timeout):
```cpp
#ifdef HAVE_LIBSYSTEMD
  {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_watchdog >= std::chrono::seconds(5)) {
      sd_notify(0, "WATCHDOG=1");
      last_watchdog = now;
    }
  }
#endif
```
Add `#include <chrono>` to the C++ standard-library includes block.

**File:** `systemd/crashomon-watcherd.service`

Change `Type=simple` → `Type=notify`, add `WatchdogSec=10s` to `[Service]`.

### Step 7 — Async minidump processing (worker thread)

**File:** `daemon/main.cpp`

Add includes: `#include <condition_variable>`, `#include <mutex>`, `#include <queue>`,
`#include <thread>` to the C++ standard-library block.

**Define `WorkerState` in anonymous namespace:**
```cpp
struct WorkerState {
  std::queue<std::string> pending;
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
};
```

**Modify `ProcessInotifyEvents`** (rename to `EnqueueInotifyEvents`): instead of calling
`ProcessNewMinidump`, push the full path string to `worker_state.pending` under the mutex and
`cv.notify_one()`. Pass `WorkerState&` and the `pending_dir` prefix to compose the full path.

**Change `ProcessNewMinidump` signature** to accept a full path `const std::string& path` directly,
removing the `pending_dir` + `name` split.

**Add `RunWorker` function:**
```cpp
void RunWorker(WorkerState& state, const crashomon::DiskManagerConfig& prune_cfg) {
  while (true) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(state.mu);
      state.cv.wait(lock, [&state] {
        return state.stop || !state.pending.empty();
      });
      if (state.stop && state.pending.empty()) return;
      path = std::move(state.pending.front());
      state.pending.pop();
    }
    ProcessNewMinidump(path, prune_cfg);
  }
}
```

**In `RunWatcher`:**
1. Declare `WorkerState worker_state;` before the poll loop.
2. Spawn: `std::thread worker_thread(RunWorker, std::ref(worker_state), std::cref(prune_cfg));`
3. Poll loop body: replace `ProcessInotifyEvents(...)` with `EnqueueInotifyEvents(..., worker_state)`.
4. Shutdown sequence (after loop exits on `g_stop`):
   - First: `close(client_fd)` + join `handler_thread` (Crashpad finishes writing dumps).
   - Then: signal worker:
     ```cpp
     { std::lock_guard<std::mutex> lock(worker_state.mu); worker_state.stop = true; }
     worker_state.cv.notify_one();
     worker_thread.join();
     ```
   - Then: existing inotify cleanup.

`worker_state.stop` is a plain `bool` accessed only under `worker_state.mu` — no `std::atomic` needed.

### Step 8 — Crash rate limiting

**File:** `daemon/main.cpp`

Add `#include <unordered_map>` and `#include <cinttypes>` (for `PRIx64`).

Add `rate_limit_map` to `WorkerState`:
```cpp
std::unordered_map<std::string, std::chrono::steady_clock::time_point> rate_limit_map;
```
No additional mutex needed — only the single worker thread accesses it.

In `ProcessNewMinidump`, after `ReadMinidump` succeeds, before `FormatTombstone`, add a rate-limit
check. Pass `WorkerState&` to `ProcessNewMinidump` (or factor the map out as a separate parameter).
The cleanest approach: add `WorkerState& state` as a parameter to `ProcessNewMinidump` and access
`state.rate_limit_map` there.

Rate limit logic:
```cpp
char fault_hex[20];
snprintf(fault_hex, sizeof(fault_hex), "%" PRIx64, info.fault_addr);
const std::string key = info.process_name + ":" + info.signal_info + ":" + fault_hex;

const auto now = std::chrono::steady_clock::now();
auto it = state.rate_limit_map.find(key);
if (it != state.rate_limit_map.end() &&
    (now - it->second) < std::chrono::seconds(30)) {
  Log("crashomon-watcherd: suppressed duplicate crash (%s)", key.c_str());
  // Still prune — the file was written to disk.
  if (const auto s = crashomon::PruneMinidumps(prune_cfg); !s.ok()) {
    Log("crashomon-watcherd: prune failed: %s", std::string(s.message()).c_str());
  }
  return;
}
state.rate_limit_map[key] = now;
```

The map is never pruned (acceptable: each entry ~100–200 bytes, 10k unique crashes ≈ 1–2MB).

### Step 9 — Web: tmp file cleanup for symbol uploads

**File:** `web/app.py`

For `upload_symbols` (~line 183) and `api_upload_symbols` (~line 223): wrap the processing in
`try/finally` to delete the tmp binary after symbols are extracted:
```python
bin_path = tmp_dir / safe_name
f.save(str(bin_path))
try:
    # existing symbol extraction logic
finally:
    bin_path.unlink(missing_ok=True)
```

For `upload_crash` and `api_upload_crash`: the `dmp_path` is stored in the database as a permanent
reference — do NOT delete it. These paths are intentionally retained as the stored minidump.

### Step 10 — Web: CSRF protection

**File:** `web/requirements.txt`  
Add: `flask-wtf>=1.2`

**File:** `pyproject.toml`  
Add `"flask-wtf>=1.2"` to `[project.dependencies]`.

**File:** `web/app.py`  
Add import: `from flask_wtf.csrf import CSRFProtect`  
After `app = Flask(...)` and `SECRET_KEY` assignment: `csrf = CSRFProtect(app)`  
Exempt the API routes (they use API-key auth, not browser sessions):
```python
@app.post("/api/symbols/upload")
@csrf.exempt
def api_upload_symbols(): ...

@app.post("/api/crashes/upload")
@csrf.exempt
def api_upload_crash(): ...
```
Note: `@csrf.exempt` must be the innermost decorator (closest to the function), applied after `@app.post`.

**Templates** — add hidden CSRF token inside every `<form method="post">`:
```html
<input type="hidden" name="csrf_token" value="{{ csrf_token() }}">
```
Files to update:
- `web/templates/upload.html` — both forms (minidump and tombstone paste)
- `web/templates/crash_detail.html` — delete form
- `web/templates/symbols.html` — symbol upload form and delete form

**File:** `web/tests/conftest.py`  
Add `a.config["WTF_CSRF_ENABLED"] = False` after `create_app(...)` in the `app` fixture so
existing tests that POST without CSRF tokens continue to pass.

---

## Execution Order

Steps must be executed in this sequence due to dependencies:

| Step | Depends on |
|------|-----------|
| 1 (clang-tidy) | — |
| 2 (strncpy) | 1 (removes associated NOLINT prose) |
| 3 (syslog) | 2 (same file, adjacent code block) |
| 4 (ScopedFD) | 1 (NOLINT removal done), 2 (strncpy done) |
| 5 (socket collision) | 4 (ScopedFD now used in CreateListenSocket) |
| 6 (sd_notify) | 4, 5 (RunWatcher changes stable) |
| 7 (async worker) | 4, 6 (poll loop shape finalized) |
| 8 (rate limiting) | 7 (worker thread exists) |
| 9 (tmp cleanup) | — (independent) |
| 10 (CSRF) | — (independent, do in one pass: requirements → app.py → templates → conftest) |

Steps 9 and 10 are fully independent of the C++ changes and can be done in any order relative to
steps 1–8.

---

## Critical Files

| File | Changes |
|------|---------|
| `.clang-tidy` | Add `-misc-include-cleaner,` |
| `daemon/main.cpp` | Steps 1–8: NOLINT removal, snprintf, ScopedFD, collision probe, sd_notify, worker thread, rate limiter |
| `daemon/CMakeLists.txt` | Step 6: PkgConfig + libsystemd |
| `lib/crashomon.cpp` | Steps 1–3: NOLINT removal, snprintf, syslog |
| `systemd/crashomon-watcherd.service` | Step 6: Type=notify, WatchdogSec=10s |
| `web/app.py` | Steps 9–10: tmp cleanup + CSRF init + exemptions |
| `web/requirements.txt` | Step 10: flask-wtf |
| `pyproject.toml` | Step 10: flask-wtf |
| `web/templates/upload.html` | Step 10: csrf_token hidden inputs |
| `web/templates/crash_detail.html` | Step 10: csrf_token hidden input |
| `web/templates/symbols.html` | Step 10: csrf_token hidden inputs |
| `web/tests/conftest.py` | Step 10: WTF_CSRF_ENABLED=False |

---

## Verification

After implementation:

1. **Build:** `cmake -B build && cmake --build build` — must compile cleanly with no warnings.
2. **Clang-tidy:** `cmake -B build-tidy -DENABLE_CLANG_TIDY=ON && cmake --build build-tidy` — no
   NOLINT-related errors; misc-include-cleaner warnings gone.
3. **C++ tests:** `ctest --test-dir build` — all 83+ tests pass.
4. **Python tests:** `python -m pytest web/tests/` from the project root — all 143+ tests pass.
5. **Manual socket collision:** Start watcherd, attempt to start a second instance — second should
   exit immediately with a clear error message.
6. **Manual syslog check:** Stop watcherd, run a test binary with `LD_PRELOAD=libcrashomon.so` —
   `journalctl -f` should show the "could not connect" warning.
7. **Systemd watchdog:** `systemd-analyze verify systemd/crashomon-watcherd.service` — no warnings.
   Start daemon, confirm `systemctl status` shows "Active: active (running)".
8. **CSRF test:** Open web UI, right-click → inspect the delete form — confirm `csrf_token` hidden
   input is present. Submit a manual POST without the token — expect 400.
9. **Ruff:** `ruff check web/` — clean.

---

## Also Write To

Create `claude_plans/2026-04-04-crashomon-assessment-improvements.md` with this content (user-facing copy, date-prefixed per project convention).
