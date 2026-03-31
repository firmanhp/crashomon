#!/usr/bin/env bash
# test/stress_test.sh — stress test for crashomon-watcherd crash capture.
#
# Floods the daemon with many concurrent crashing programs and verifies
# that every minidump is captured without any being missed.
#
# Uses all three example crashers (abort, segfault, multithread) in
# round-robin order to exercise different signal types, stack depths,
# thread counts, and minidump sizes.
#
# Prerequisites:
#   cmake -B build && cmake --build build
#
# Usage:
#   test/stress_test.sh [BUILD_DIR] [OPTIONS]
#
#   -n, --total N       Total number of crashes to send (default: 2000)
#   -b, --batch N       Crashes per batch (default: 40)
#   -p, --pause SECS    Pause between batches (default: 0.1)
#   -t, --timeout SECS  Max seconds to wait for all dumps (default: 300)
#
# Exit code: 0 = all dumps captured; non-zero = failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Defaults ──────────────────────────────────────────────────────────────────

TOTAL=2000
BATCH_SIZE=20
PAUSE_SECS=0.5
TIMEOUT_SECS=300

# ── Argument parsing ─────────────────────────────────────────────────────────

BUILD_DIR=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -n|--total)   TOTAL="$2"; shift 2 ;;
    -b|--batch)   BATCH_SIZE="$2"; shift 2 ;;
    -p|--pause)   PAUSE_SECS="$2"; shift 2 ;;
    -t|--timeout) TIMEOUT_SECS="$2"; shift 2 ;;
    -*)           echo "Unknown option: $1" >&2; exit 1 ;;
    *)
      if [[ -z "${BUILD_DIR}" ]]; then
        BUILD_DIR="$1"; shift
      else
        echo "Unexpected argument: $1" >&2; exit 1
      fi
      ;;
  esac
done

BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
BUILD_DIR="$(realpath "${BUILD_DIR}")"

# ── Locate artifacts ─────────────────────────────────────────────────────────

LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
EXAMPLES_BIN="${BUILD_DIR}/examples"

CRASHERS=(
  "${EXAMPLES_BIN}/crashomon-example-abort"
  "${EXAMPLES_BIN}/crashomon-example-segfault"
  "${EXAMPLES_BIN}/crashomon-example-multithread"
)

for artifact in "${LIBCRASHOMON}" "${WATCHERD}" "${CRASHERS[@]}"; do
  if [[ ! -f "${artifact}" ]]; then
    echo "FATAL: Missing artifact: ${artifact}" >&2
    echo "Run: cmake -B build && cmake --build build" >&2
    exit 1
  fi
done

# ── Work directory ────────────────────────────────────────────────────────────

WORK_DIR="$(mktemp -d)"
DB_DIR="${WORK_DIR}/crashdb"
SOCKET_PATH="${WORK_DIR}/handler.sock"
mkdir -p "${DB_DIR}"

cleanup() {
  if [[ -n "${WATCHERD_PID:-}" ]]; then
    kill "${WATCHERD_PID}" 2>/dev/null || true
    wait "${WATCHERD_PID}" 2>/dev/null || true
  fi
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# ── Print header ──────────────────────────────────────────────────────────────

echo "=== Crashomon Stress Test ==="
echo "Build dir:  ${BUILD_DIR}"
echo "Crashers:   abort, segfault, multithread (round-robin)"
echo "Total:      ${TOTAL}"
echo "Batch size: ${BATCH_SIZE}"
echo "Pause:      ${PAUSE_SECS}s"
echo "Timeout:    ${TIMEOUT_SECS}s"
echo ""

# ── Disk space check ──────────────────────────────────────────────────────────

free_kb=$(df -k "${WORK_DIR}" | awk 'NR==2{print $4}')
if [[ "${free_kb}" -lt 102400 ]]; then
  echo "WARNING: Less than 100MB free disk space (${free_kb}KB)" >&2
fi

# ── Start watcherd ────────────────────────────────────────────────────────────

echo "-- Starting watcherd --"

"${WATCHERD}" \
  --db-path="${DB_DIR}" \
  --socket-path="${SOCKET_PATH}" \
  >"${WORK_DIR}/watcherd.log" 2>&1 &
WATCHERD_PID=$!

# Poll for socket readiness.
socket_ready=false
for _ in $(seq 1 10); do
  if [[ -S "${SOCKET_PATH}" ]]; then
    socket_ready=true
    break
  fi
  sleep 0.5
done

if [[ "${socket_ready}" != "true" ]]; then
  echo "FATAL: watcherd socket did not appear within 5s" >&2
  tail -20 "${WORK_DIR}/watcherd.log" >&2
  exit 1
fi

echo "  watcherd started (pid ${WATCHERD_PID})"
echo ""

# ── Send crashes ──────────────────────────────────────────────────────────────

echo "-- Sending crashes --"

START_TIME=$(date +%s)
NUM_CRASHERS=${#CRASHERS[@]}
launched=0
batch_num=0

while [[ ${launched} -lt ${TOTAL} ]]; do
  batch_num=$((batch_num + 1))
  batch_end=$((launched + BATCH_SIZE))
  if [[ ${batch_end} -gt ${TOTAL} ]]; then
    batch_end=${TOTAL}
  fi

  # Launch this batch in parallel.
  pids=()
  for ((i = launched; i < batch_end; i++)); do
    crasher="${CRASHERS[ i % NUM_CRASHERS ]}"
    LD_PRELOAD="${LIBCRASHOMON}" \
      CRASHOMON_SOCKET_PATH="${SOCKET_PATH}" \
      "${crasher}" >/dev/null 2>&1 &
    pids+=($!)
  done

  # Wait for all crashers in this batch to exit.
  for pid in "${pids[@]}"; do
    wait "${pid}" 2>/dev/null || true
  done

  launched=${batch_end}

  # Progress every 10 batches or at the end.
  if (( batch_num % 10 == 0 )) || [[ ${launched} -eq ${TOTAL} ]]; then
    current_dumps=$(find "${DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null | wc -l)
    elapsed=$(( $(date +%s) - START_TIME ))
    echo "  [${elapsed}s] Launched: ${launched}/${TOTAL}  Captured: ${current_dumps}"
  fi

  # Health check.
  if ! kill -0 "${WATCHERD_PID}" 2>/dev/null; then
    echo "FATAL: watcherd died during stress test" >&2
    echo "Last 20 lines of watcherd log:" >&2
    tail -20 "${WORK_DIR}/watcherd.log" >&2
    exit 1
  fi

  sleep "${PAUSE_SECS}"
done

echo ""

# ── Wait for all minidumps ────────────────────────────────────────────────────

echo "-- Waiting for remaining minidumps --"

deadline=$(( $(date +%s) + TIMEOUT_SECS ))

while true; do
  count=$(find "${DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null | wc -l)

  if [[ ${count} -ge ${TOTAL} ]]; then
    echo "  All ${TOTAL} minidumps captured."
    break
  fi

  now=$(date +%s)
  if [[ ${now} -ge ${deadline} ]]; then
    echo "TIMEOUT: Only ${count}/${TOTAL} dumps after ${TIMEOUT_SECS}s" >&2
    # Diagnostic: check new/ for stuck dumps.
    new_count=$(find "${DB_DIR}/new" -name '*.dmp' -type f 2>/dev/null | wc -l)
    if [[ ${new_count} -gt 0 ]]; then
      echo "  ${new_count} dumps stuck in new/ (not moved to pending/)" >&2
    fi
    echo "Last 20 lines of watcherd log:" >&2
    tail -20 "${WORK_DIR}/watcherd.log" >&2
    break
  fi

  # Health check.
  if ! kill -0 "${WATCHERD_PID}" 2>/dev/null; then
    echo "FATAL: watcherd died while waiting for dumps (${count}/${TOTAL})" >&2
    tail -20 "${WORK_DIR}/watcherd.log" >&2
    break
  fi

  remaining=$((deadline - now))
  echo "  Waiting... ${count}/${TOTAL} dumps (${remaining}s remaining)"
  sleep 2
done

echo ""

# ── Results ───────────────────────────────────────────────────────────────────

final_count=$(find "${DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null | wc -l)
elapsed=$(( $(date +%s) - START_TIME ))
total_size=$(du -sh "${DB_DIR}/pending" 2>/dev/null | cut -f1)

echo "=== Results ==="
echo "Target:   ${TOTAL}"
echo "Captured: ${final_count}"
echo "Elapsed:  ${elapsed}s"
echo "Size:     ${total_size}"
if [[ ${elapsed} -gt 0 ]]; then
  rate=$(echo "scale=1; ${final_count} / ${elapsed}" | bc 2>/dev/null || echo "?")
  echo "Rate:     ${rate} dumps/sec"
fi

if [[ ${final_count} -eq ${TOTAL} ]]; then
  echo "PASS"
  exit 0
else
  echo "FAIL: Expected ${TOTAL}, got ${final_count} (missed $((TOTAL - final_count)))" >&2
  exit 1
fi
