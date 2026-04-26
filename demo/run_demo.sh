#!/usr/bin/env bash
# run_demo.sh — end-to-end crashomon demonstration
#
# Demonstrates the full crashomon workflow on a native (non-cross) Linux host:
#
#   1. Build    — host tools (dump_syms) + crashomon + demo-crasher via CMake
#   2. Symbols  — crashomon-syms: ingest app binary, ingest host sysroot (/usr/lib),
#                  list store contents, prune old versions
#   3. Crash    — start watcherd, run demo-crasher (it crashes), capture minidump
#                  and watcherd tombstone log, stop watcherd
#   4. Analyze  — four crashomon-analyze modes:
#                  a) --minidump only          (raw, unsymbolicated)
#                  b) --minidump --store        (app symbols only, partial)
#                  c) --minidump --store        (full store with sysroot, complete)
#                  d) --minidump --sysroot      (lazy per-minidump sysroot extraction)
#
# Usage:
#   ./run_demo.sh [--clean]
#
#   --clean   Wipe all build and work directories before starting.
#
# First run downloads Crashpad, Abseil and other FetchContent deps (~300 MB).
# Subsequent runs use the cached source trees and rebuild only changed files.
# The sysroot scan (step 2b) may take a few minutes on a large /usr/lib.

set -euo pipefail

# ── Paths ──────────────────────────────────────────────────────────────────────
DEMO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$DEMO_DIR")"

HOST_BUILD="$DEMO_DIR/_host_build"     # standalone dump_syms build
DEMO_BUILD="$DEMO_DIR/_build"          # full demo CMake build
WORK_DIR="$DEMO_DIR/_work"             # runtime artifacts
MINIDUMP_DIR="$WORK_DIR/minidumps"     # .dmp files written by watcherd
SYMBOLS_FULL="$WORK_DIR/symbols"       # app + bulk-ingested sysroot symbols
SYMBOLS_APP="$WORK_DIR/symbols_app"    # app symbols only (for lazy-extraction demo)
LOG_DIR="$WORK_DIR/logs"               # watcherd stdout/stderr
# Demo sysroot: a minimal flat layout mirroring a cross-compilation sysroot.
# crashomon-syms --sysroot and crashomon-analyze --sysroot both scan
# <sysroot>/usr/lib/ (top-level, non-recursive) — the standard layout for
# cross-compiled sysroots.  On Ubuntu, the system libraries are in
# /usr/lib/x86_64-linux-gnu/ which is NOT scanned.  We build a minimal sysroot
# containing only the libraries that appear in the crash so the demo works
# correctly on the host machine.
DEMO_SYSROOT="$WORK_DIR/sysroot"

SYMS_TOOL="$ROOT_DIR/_host_toolkit/bin/crashomon-syms"
[[ ! -f "$SYMS_TOOL" ]] && SYMS_TOOL="$ROOT_DIR/tools/syms/crashomon-syms"
ANALYZE_TOOL="$ROOT_DIR/tools/analyze/crashomon-analyze"

# ── Terminal helpers ───────────────────────────────────────────────────────────
_bold()   { printf '\033[1m%s\033[0m\n'       "$*"; }
_step()   { printf '\n\033[1;34m══ %s ══\033[0m\n' "$*"; }
_ok()     { printf '\033[1;32m✓\033[0m  %s\n'  "$*"; }
_warn()   { printf '\033[1;33m!\033[0m  %s\n'  "$*"; }
_die()    { printf '\033[1;31m✗\033[0m  %s\n'  "$*" >&2; exit 1; }
_banner() { printf '\n\033[1;36m%s\033[0m\n'   "$*"; }

# ── Cleanup: kill watcherd on script exit (normal or error) ───────────────────
WATCHERD_PID=""
SOCKET_PATH="$WORK_DIR/crashomon.sock"
cleanup() {
    if [[ -n "$WATCHERD_PID" ]]; then
        kill "$WATCHERD_PID" 2>/dev/null || true
        wait "$WATCHERD_PID" 2>/dev/null || true
        WATCHERD_PID=""
    fi
    rm -f "$SOCKET_PATH"
}
trap cleanup EXIT

# ── Option parsing ─────────────────────────────────────────────────────────────
if [[ "${1:-}" == "--clean" ]]; then
    _step "Cleaning previous build and work directories"
    rm -rf "$HOST_BUILD" "$DEMO_BUILD" "$WORK_DIR"
    _ok "Clean done"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 1: BUILD
# ═══════════════════════════════════════════════════════════════════════════════
_step "Phase 1 · Build"

# ── 1a. Host tools: dump_syms, minidump-stackwalk, crashomon-analyze ───────────
# cmake/host_toolkit/ builds all three tools in one step.  Outputs land in
# _host_build/bin/.  Built with the host compiler; safe to reuse across
# cross-compilation builds.
if [[ -x "$HOST_BUILD/bin/dump_syms" ]]; then
    _ok "Host tools already built ($HOST_BUILD/bin/)"
else
    echo "  Building host tools..."
    cmake -S "$ROOT_DIR/cmake/host_toolkit" -B "$HOST_BUILD" -Wno-dev \
        2>&1 | grep -v '^--' | tail -8 || true
    cmake --build "$HOST_BUILD" --target host_toolkit --parallel "$(nproc)"
    _ok "Host tools built: $HOST_BUILD/bin/"
fi

DUMP_SYMS_BIN="$HOST_BUILD/bin/dump_syms"
[[ -x "$DUMP_SYMS_BIN" ]] || _die "dump_syms not found at $DUMP_SYMS_BIN"

# ── 1b. Full crashomon build + demo-crasher ────────────────────────────────────
# The demo's CMakeLists.txt uses add_subdirectory(..) to pull in the full
# crashomon tree, building crashomon_client and crashomon-watcherd alongside
# demo-crasher. Host tools are supplied via CRASHOMON_HOST_TOOLKIT_DIR.
echo "  Configuring and building crashomon + demo-crasher..."
cmake -S "$DEMO_DIR" -B "$DEMO_BUILD" -Wno-dev \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$HOST_BUILD/bin" \
    2>&1 | grep -v '^--' | tail -8 || true
cmake --build "$DEMO_BUILD" --parallel "$(nproc)"

# Source the generated paths file.  file(GENERATE) writes it at CMake generate
# time (before compilation), and the paths remain valid after --build finishes.
# shellcheck source=/dev/null
source "$DEMO_BUILD/demo_paths.sh"

[[ -x "$DEMO_CRASHER"       ]] || _die "demo-crasher not found at $DEMO_CRASHER"
[[ -x "$WATCHERD"           ]] || _die "crashomon-watcherd not found at $WATCHERD"
[[ -x "$MINIDUMP_STACKWALK" ]] || _die "minidump_stackwalk not found at $MINIDUMP_STACKWALK"
[[ -x "$DUMP_SYMS"          ]] || _die "dump_syms not found at $DUMP_SYMS"

_ok "Build complete"
echo "    demo-crasher:       $DEMO_CRASHER"
echo "    crashomon-watcherd: $WATCHERD"
echo "    minidump_stackwalk: $MINIDUMP_STACKWALK"
echo "    dump_syms:          $DUMP_SYMS"

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 2: CAPTURE THE CRASH
# ═══════════════════════════════════════════════════════════════════════════════
_step "Phase 2 · Capture crash"

# Prepare fresh work directories each run.
rm -rf "$WORK_DIR"
mkdir -p "$MINIDUMP_DIR" "$SYMBOLS_FULL" "$SYMBOLS_APP" "$LOG_DIR"

# ── Start crashomon-watcherd ──────────────────────────────────────────────────
# The watcherd hosts the Crashpad ExceptionHandlerServer, accepts client
# connections via the Unix socket, writes minidumps to MINIDUMP_DIR, and
# prints Android-style tombstones (with BuildId annotations) to stdout.
# (FormatTombstone uses std::fwrite to stdout; spdlog daemon logs go to stderr.)
echo "  Starting crashomon-watcherd..."
# stdout  → tombstone.log  (FormatTombstone writes to stdout)
# stderr  → daemon.log    (spdlog structured logs)
"$WATCHERD" \
    --db-path="$MINIDUMP_DIR" \
    --socket-path="$SOCKET_PATH" \
    --max-size=100M \
    --max-age=7d \
    >"$LOG_DIR/tombstone.log" 2>"$LOG_DIR/daemon.log" &
WATCHERD_PID=$!

# Poll until the socket file appears (daemon is ready).
for i in $(seq 1 20); do
    [[ -S "$SOCKET_PATH" ]] && break
    sleep 0.25
done
[[ -S "$SOCKET_PATH" ]] || \
    _die "watcherd socket did not appear after 5 s — check $LOG_DIR/daemon.log"
_ok "watcherd started (PID $WATCHERD_PID, socket: $SOCKET_PATH)"

# ── Run demo-crasher ──────────────────────────────────────────────────────────
# CRASHOMON_SOCKET_PATH directs the client library to our local watcherd socket
# instead of the system default.  LD_LIBRARY_PATH ensures the dynamic linker
# finds libcrashomon.so from the build tree (the RPATH should already cover
# this, but LD_LIBRARY_PATH is a safety net for unusual linker configurations).
echo "  Running demo-crasher (crash expected on record 3)..."
CRASHOMON_SOCKET_PATH="$SOCKET_PATH" \
LD_LIBRARY_PATH="$LIBCRASHOMON_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$DEMO_CRASHER" 2>&1 || true   # non-zero exit is expected — it crashes

# ── Wait for the minidump ─────────────────────────────────────────────────────
# Crashpad writes the .dmp file out-of-process (inside the watcherd) so it may
# lag by a fraction of a second behind the client's crash.
DMP_FILE=""
for i in $(seq 1 40); do
    DMP_FILE="$(find "$MINIDUMP_DIR" -name '*.dmp' 2>/dev/null | head -1)"
    [[ -n "$DMP_FILE" ]] && break
    sleep 0.5
done
[[ -n "$DMP_FILE" ]] || \
    _die "No .dmp written after 20 s.  Is the watcherd socket reachable?"
_ok "Minidump written: $DMP_FILE"

# ── Wait for the tombstone ────────────────────────────────────────────────────
# The watcherd processes new minidumps via inotify and writes the tombstone
# (with BuildId annotations) to stdout (tombstone.log).  Wait until the
# separator line appears, meaning the tombstone has been fully written.
for i in $(seq 1 40); do
    grep -qF '*** *** ***' "$LOG_DIR/tombstone.log" 2>/dev/null && break
    sleep 0.5
done

if grep -qF '*** *** ***' "$LOG_DIR/tombstone.log" 2>/dev/null; then
    _ok "Tombstone written to $LOG_DIR/tombstone.log"
else
    _warn "Tombstone not found in watcherd log."
fi

# Stop watcherd gracefully now that crash data is captured.
kill "$WATCHERD_PID" 2>/dev/null || true
wait "$WATCHERD_PID" 2>/dev/null || true
WATCHERD_PID=""
_ok "watcherd stopped"

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 3: SYMBOL INGESTION  (crashomon-syms)
# ═══════════════════════════════════════════════════════════════════════════════
_step "Phase 3 · Symbol ingestion"

# ── 3a. Ingest the demo-crasher binary into both stores ───────────────────────
# crashomon-syms add runs dump_syms on the ELF and stores the .sym file using
# the Breakpad layout: <store>/<module_name>/<build_id>/<module_name>.sym
echo "  [3a] Ingesting demo-crasher symbols..."
"$SYMS_TOOL" add \
    --store "$SYMBOLS_FULL" \
    --dump-syms "$DUMP_SYMS" \
    "$DEMO_CRASHER"

# SYMBOLS_APP gets only the app binary — used later in the lazy-sysroot demo.
"$SYMS_TOOL" add \
    --store "$SYMBOLS_APP" \
    --dump-syms "$DUMP_SYMS" \
    "$DEMO_CRASHER"
_ok "App binary symbols stored"

# ── 3b. Build demo sysroot + bulk-ingest sysroot symbols ─────────────────────
# crashomon-syms --sysroot and crashomon-analyze --sysroot both scan
# <sysroot>/usr/lib/ (top-level, non-recursive) — the standard layout for
# cross-compiled sysroots.  On Ubuntu hosts, system libraries live in
# /usr/lib/x86_64-linux-gnu/ and are NOT found by a plain --sysroot /.
#
# We build a minimal demo sysroot containing only the libraries referenced by
# the minidump (libc.so.6 and its peer .so files in the same arch dir).  This
# gives the demo a realistic sysroot layout without scanning thousands of files.
# In real cross-compilation workflows the sysroot already has a flat usr/lib/.
echo "  [3b] Building minimal demo sysroot from /usr/lib/x86_64-linux-gnu..."
mkdir -p "$DEMO_SYSROOT/usr/lib"
# Copy shared libraries that appear in the crash backtrace.
for lib in /usr/lib/x86_64-linux-gnu/libc.so.6 \
           /usr/lib/x86_64-linux-gnu/libm.so.6 \
           /usr/lib/x86_64-linux-gnu/libpthread.so.0 \
           /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2; do
    [[ -f "$lib" ]] && cp "$lib" "$DEMO_SYSROOT/usr/lib/" && echo "    added $(basename "$lib")"
done
echo ""
echo "  [3b] Running crashomon-syms add --sysroot (bulk ingest)..."
"$SYMS_TOOL" add \
    --store "$SYMBOLS_FULL" \
    --dump-syms "$DUMP_SYMS" \
    --sysroot "$DEMO_SYSROOT"
_ok "Sysroot symbols stored"

# ── 3c. List store contents ────────────────────────────────────────────────────
echo "  [3c] Full symbol store contents:"
"$SYMS_TOOL" list --store "$SYMBOLS_FULL"

echo ""
echo "  [3c] App-only store contents:"
"$SYMS_TOOL" list --store "$SYMBOLS_APP"

# ── 3d. Prune the store ────────────────────────────────────────────────────────
# Keep at most 3 versions per module (oldest removed first by mtime).
# With a single build per module this is a no-op, but it demonstrates the
# command that CI/CD pipelines use to prevent unbounded store growth.
echo "  [3d] Pruning symbol store (keep=3)..."
"$SYMS_TOOL" prune --store "$SYMBOLS_FULL" --keep 3
_ok "Store pruned"

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 4: ANALYSIS  (crashomon-analyze)
# ═══════════════════════════════════════════════════════════════════════════════
_step "Phase 4 · Analysis"

# ── 4a. Mode: --minidump only (raw, unsymbolicated) ───────────────────────────
_banner "── 4a. Raw minidump (no symbol store) ──"
echo "  Shows frame addresses without function names."
"$ANALYZE_TOOL" \
    --minidump="$DMP_FILE" \
    --stackwalk-binary="$MINIDUMP_STACKWALK"
echo ""

# ── 4b. Mode: --minidump --store (app symbols only) ──────────────────────────
_banner "── 4b. Minidump + app-only store ──"
echo "  App frames (demo-crasher) are symbolicated."
echo "  System library frames remain as raw addresses."
"$ANALYZE_TOOL" \
    --store="$SYMBOLS_APP" \
    --minidump="$DMP_FILE" \
    --stackwalk-binary="$MINIDUMP_STACKWALK"
echo ""

# ── 4c. Mode: --minidump --store (full store: app + sysroot) ─────────────────
_banner "── 4c. Minidump + full store (app + pre-ingested sysroot) ──"
echo "  All frames fully symbolicated using the pre-built symbol store."
"$ANALYZE_TOOL" \
    --store="$SYMBOLS_FULL" \
    --minidump="$DMP_FILE" \
    --stackwalk-binary="$MINIDUMP_STACKWALK"
echo ""

# ── 4d. Mode: --minidump --store --sysroot (lazy sysroot extraction) ──────────
_banner "── 4d. Minidump + lazy sysroot extraction ──"
echo "  Starts with the app-only store."
echo "  dump_syms is run on-demand for unresolved sysroot libraries."
echo "  Extracted .sym files are cached in the store for future runs."
"$ANALYZE_TOOL" \
    --store="$SYMBOLS_APP" \
    --sysroot="$DEMO_SYSROOT" \
    --minidump="$DMP_FILE" \
    --stackwalk-binary="$MINIDUMP_STACKWALK" \
    --dump-syms-binary="$DUMP_SYMS"
echo ""

# Show that the lazy-extracted symbols were added to the app-only store.
echo "  App-only store after lazy extraction:"
"$SYMS_TOOL" list --store "$SYMBOLS_APP"
echo ""

# ═══════════════════════════════════════════════════════════════════════════════
echo ""
_ok "Demo complete."
echo ""
echo "  Minidump:           $DMP_FILE"
echo "  Tombstone log:      $LOG_DIR/tombstone.log"
echo "  Daemon log:         $LOG_DIR/daemon.log"
echo "  Full symbol store:  $SYMBOLS_FULL"
echo "  App symbol store:   $SYMBOLS_APP"
