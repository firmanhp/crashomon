# Stress Test: Daemon Crash Capture at Scale

## Context

The daemon (`crashomon-watcherd`) must reliably capture every crash minidump under load. This plan adds `test/stress_test.sh` — a shell script that floods the daemon with 2000 concurrent crashing programs and verifies that every minidump is captured without any being missed.

---

## Architecture Understanding

### Crash capture pipeline
1. Crasher process loads `libcrashomon.so` via `LD_PRELOAD`, connects to watcherd's Unix socket.
2. Watcherd accepts the connection, spawns a detached thread running `ExceptionHandlerServer`.
3. The handler thread ptrace-attaches to the crashed process and writes a minidump to `new/<uuid>.dmp`.
4. Crashpad atomically renames the file to `pending/<uuid>.dmp`.
5. Watcherd's main loop detects the inotify `IN_MOVED_TO` event, calls `ProcessNewMinidump()`.
6. ProcessNewMinidump reads the minidump, formats a tombstone, and optionally prunes old dumps.

### Key constraints
- **Listen backlog**: 128 pending connections (`kListenBacklog`).
- **Accept rate**: One `accept4()` per poll-loop iteration; loop returns immediately when listen fd is readable.
- **Minidump processing**: Synchronous in the main thread — each inotify event is handled serially.
- **Threading**: Unbounded detached threads (one per client).
- **Minidump sizes**: ~13KB (abort), ~15KB (segfault), ~25KB (multithread). Weighted average ~18KB. 2000 × 18KB ≈ 36MB disk.
- **multithread crasher** has a 50ms sleep before crashing — this means handler threads stay alive longer, increasing peak concurrent thread count. This is desirable for stress testing.

### Batching rationale
Sending 2000 connections simultaneously would overflow the listen backlog (128) and exhaust memory with 2000 thread stacks (~4GB). Sending in batches of 40 stays well within the backlog, allows the daemon to accept and spawn threads between waves, and keeps concurrent thread count manageable (~40 at peak).

---

## Implementation

### New file: `test/stress_test.sh`

Single shell script (~150 lines) following the conventions of `test/integration_test.sh`.

### Script structure

```
parse_args()          — CLI: BUILD_DIR, -n/--total, -b/--batch, -t/--timeout
locate_artifacts()    — find libcrashomon.so, watcherd, all 3 crasher binaries
setup_workdir()       — mktemp -d, mkdir db dir, trap EXIT cleanup
start_watcherd()      — launch watcherd, poll socket readiness
run_stress_test()     — batch loop: launch crashers, wait per batch, progress reporting
wait_for_dumps()      — poll pending/ until count == total or timeout
verify_results()      — final count check, pass/fail, timing summary
```

### Defaults
- **Total crashes**: 2000
- **Batch size**: 40
- **Inter-batch pause**: 0.1s
- **Timeout for all dumps**: 300s (5 minutes)
- **Crasher binaries**: all three types, round-robin per crash:
  - `crashomon-example-abort` (~13KB dumps, SIGABRT)
  - `crashomon-example-segfault` (~15KB dumps, SIGSEGV null deref, 5-frame stack)
  - `crashomon-example-multithread` (~25KB dumps, SIGSEGV on worker thread after 50ms sleep)
- **Pruning**: disabled (no `--max-size`, no `--max-age`)

### Crasher selection

All three example binaries are used in round-robin order within each batch. This exercises different crash types (SIGABRT, SIGSEGV single-thread, SIGSEGV multi-thread), different minidump sizes, and different thread counts — stressing the daemon with realistic variety.

```bash
CRASHERS=(
    "${EXAMPLES_BIN}/crashomon-example-abort"
    "${EXAMPLES_BIN}/crashomon-example-segfault"
    "${EXAMPLES_BIN}/crashomon-example-multithread"
)
```

### Batch loop design

```bash
while launched < total:
    # Launch batch_size crashers in parallel, cycling through crash types
    for i in batch:
        crasher="${CRASHERS[ i % ${#CRASHERS[@]} ]}"
        LD_PRELOAD=libcrashomon.so CRASHOMON_SOCKET_PATH=... "$crasher" &
        pids+=($!)

    # Wait for all crashers in this batch to exit
    for pid in pids:
        wait $pid || true   # crashers exit with signals (non-zero)

    launched += batch_size

    # Progress every 10 batches
    if batch_num % 10 == 0:
        count = find pending/ -name '*.dmp' | wc -l
        print "[Ns] Launched: X/Y  Captured: Z"

    # Health check: is watcherd still alive?
    kill -0 $WATCHERD_PID || FATAL

    sleep $pause
```

### Wait-for-dumps logic

After all crashers have been launched and waited on:

```bash
deadline = now + timeout
while count < total and now < deadline:
    count = find pending/ -name '*.dmp' | wc -l
    if count >= total: PASS
    check watcherd alive
    print "Waiting... count/total (Ns remaining)"
    sleep 2
```

### Verification

```bash
final_count = find pending/ -name '*.dmp' | wc -l
if final_count == total:
    PASS
else:
    FAIL — print count, check new/ for stuck dumps, print watcherd log tail
```

### Error handling
- **Watcherd dies**: Checked after each batch and during the wait loop. Print last 20 lines of log.
- **Disk space**: Pre-flight check — warn if <100MB free.
- **Socket not ready**: Fail if socket doesn't appear within 5s.
- **Timeout**: Print how many captured vs expected, plus diagnostic on `new/` (stuck dumps).

### Example output

```
=== Crashomon Stress Test ===
Build dir:  ./build
Crashers:   abort, segfault, multithread (round-robin)
Total:      2000
Batch size: 40
Timeout:    300s

-- Starting watcherd --
  watcherd started (pid 12345)

-- Sending crashes --
  [3s]  Launched: 400/2000   Captured: 312
  [7s]  Launched: 800/2000   Captured: 698
  [11s] Launched: 1200/2000  Captured: 1089
  [15s] Launched: 1600/2000  Captured: 1501
  [18s] Launched: 2000/2000  Captured: 1887

-- Waiting for remaining minidumps --
  Waiting... 1998/2000 dumps (280s remaining)
  All 2000 minidumps captured.

=== Results ===
Target:   2000
Captured: 2000
Elapsed:  22s
Rate:     90.9 dumps/sec
PASS
```

---

## Verification plan

```bash
# Build
cmake -B build && cmake --build build

# Run stress test with defaults (2000 crashes)
test/stress_test.sh

# Run with custom parameters
test/stress_test.sh build -n 500 -b 20 -t 120
```

---

## Files to create/modify

| File | Action |
|------|--------|
| `test/stress_test.sh` | **Create** — the stress test script (~150 lines) |
