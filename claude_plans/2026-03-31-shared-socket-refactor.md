# Plan: Refactor watcherd to single ExceptionHandlerServer with shared socket [IMPLEMENTED]

## Context

The daemon currently creates a **new ExceptionHandlerServer per accepted connection**, each on a
detached `std::thread`. This is wrong. Per Crashpad's design doc
(<https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md#detailed-design-registration-linux_android>),
shared connections are the intended mode for long-lived handlers: a single `ExceptionHandlerServer`
with `multiple_clients=true` handles all crashes on one thread. The socket pair endpoint is shared
among clients via any convenient means (e.g. inheritance or SCM_RIGHTS fd passing).

The current per-connection-thread model wastes resources and diverges from Crashpad's intended
architecture. Each detached thread runs its own `ExceptionHandlerServer` instance, which is
unnecessary overhead.

**Correct architecture**: Daemon creates a `socketpair()`. Server end goes to a single
`ExceptionHandlerServer` on a dedicated thread. Client end is shared with connecting processes via
`SCM_RIGHTS` fd passing over the accepted connection. No thread pool or per-client threads needed.

---

## Changes

### 1. `daemon/main.cpp` — Major refactor

**Remove:**
- `AcceptAndHandleClient` function (lines 224-250), which spawns a detached `std::thread` per
  client, each running its own `ExceptionHandlerServer`

**Add:**
- `socketpair(AF_UNIX, SOCK_SEQPACKET, 0)` in `RunWatcher`, before the poll loop
- A dedicated thread running `ExceptionHandlerServer::InitializeWithClient(server_fd, true)` +
  `Run(&crash_handler)`
- Daemon keeps a dup of `client_fd` alive so `Run()` does not exit (Run exits when all holders of
  the client end close their copies)
- `SO_PASSCRED` on the server-end fd before passing to `InitializeWithClient` (verify whether
  Crashpad sets this internally; if so, this step is unnecessary)

**Add `AcceptAndShareSocket` (replaces `AcceptAndHandleClient`):**

```cpp
// Accept one registration connection and send the shared client fd + daemon PID
// to the registering process via SCM_RIGHTS, then close the accepted fd.
void AcceptAndShareSocket(int listen_fd, int shared_client_fd, pid_t handler_pid);
```

Protocol over the accepted connection:
- iovec payload: `pid_t handler_pid` (4 bytes) — use `getpid()` from the daemon (Linux threads
  share a PID, so the handler thread's PID is the daemon's PID)
- cmsg: `SOL_SOCKET` / `SCM_RIGHTS` / `int shared_client_fd` (kernel dups the fd for the recipient)

**Poll loop change:** On POLLIN for listen_fd, call `AcceptAndShareSocket` — no thread dispatch,
no blocking.

**Shutdown:** `close(client_fd_copy)` -> `server.Stop()` -> join handler thread.

---

### 2. `lib/crashomon.cpp` — Two-phase socket init

**Current flow (`DoInit`):**
1. `socket()` + `connect()` to daemon's listen path
2. `setsockopt(SO_PASSCRED)` on connected fd
3. `SetHandlerSocket(conn_fd, pid=-1)` — discovers PID via `kTypeCheckCredentials` exchange

**New flow (`DoInit`):**
1. `socket()` + `connect()` to daemon's listen path
2. `recvmsg()` with `SCM_RIGHTS` to receive `shared_fd` + `handler_pid` from daemon
3. `close(conn_fd)` — registration exchange is done
4. `setsockopt(shared_fd, SO_PASSCRED)`
5. `SetHandlerSocket(shared_fd, handler_pid)` — explicit PID skips `kTypeCheckCredentials` exchange

Using explicit `handler_pid` (not -1) is required: with a shared socket, the
`kTypeCheckCredentials` reply could be consumed by the wrong process.

---

### 3. `test/stress_test.sh` — Update comments

- Update architecture comments to reflect single-server model (if any exist)

---

## Why this works

| Property | Explanation |
|---|---|
| No resource exhaustion | Single ExceptionHandlerServer thread, no per-client threads |
| Sequential crash handling | Crashpad processes crash requests one at a time via epoll — intended behavior |
| Back-pressure | Crash messages from registered clients queue in the SOCK_SEQPACKET kernel buffer |
| No lost registrations | Unregistered clients wait in kernel listen backlog (kListenBacklog=128) |
| Clean shutdown | Close client fd copy -> server.Stop() -> server exits naturally |

---

## Open questions to resolve during implementation

1. **`SO_PASSCRED` on server-end fd**: Verify whether `ExceptionHandlerServer` sets `SO_PASSCRED`
   internally. If not, the daemon must set it on `server_fd` before `InitializeWithClient`. Check
   `exception_handler_server.cc` in the Crashpad source.

2. **Backpressure under load**: With a single `ExceptionHandlerServer`, concurrent crashes queue in
   the kernel socket buffer. The stress test (2000 crashes, 40/batch) should validate this is not a
   bottleneck.

3. **Crashpad's `UnixCredentialSocket`**: Could reuse Crashpad's existing `SendMsg`/`RecvMsg` for
   `SCM_RIGHTS` instead of raw syscalls. Trade-off: less custom code vs dependency on Crashpad
   internals. Raw syscalls are fine and more explicit.

---

## Verification

```bash
cmake -B build && cmake --build build
ctest --test-dir build
test/integration_test.sh
test/demo_crash.sh
test/stress_test.sh -n 200 -b 40   # quick smoke test
test/stress_test.sh                  # full 2000-crash run
```
