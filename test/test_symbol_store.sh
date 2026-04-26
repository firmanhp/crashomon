#!/usr/bin/env bash
# test/test_symbol_store.sh — integration test for the cmake symbol store pipeline.
#
# Validates the full end-to-end flow:
#   1. cmake build triggers crashomon_store_symbols() POST_BUILD hook
#   2. Symbol store has the correct Breakpad layout
#   3. crashomon-syms can read the store
#   4. A live crash produces a .dmp that crashomon-analyze symbolicates correctly
#      using the cmake-generated store (no manual crashomon-syms add step)
#
# Usage:
#   test/test_symbol_store.sh [BUILD_DIR]
#
# Exit code: 0 = all checks passed; non-zero = at least one check failed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
BUILD_DIR="$(realpath "${BUILD_DIR}")"

PASS=0
FAIL=0

log_pass() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
log_fail() { echo "  FAIL: $*" >&2; FAIL=$((FAIL + 1)); }

echo "=== Symbol store integration test ==="
echo "Build dir: ${BUILD_DIR}"
echo ""

# ── Section 1: Build ──────────────────────────────────────────────────────────
# ENABLE_TESTS=ON pulls in the examples subdirectory which contains the
# example_segfault target. Its POST_BUILD hook stores symbols automatically.

echo "-- Build --"
cmake -B "${BUILD_DIR}" -DENABLE_TESTS=ON --log-level=WARNING 2>&1 | sed 's/^/  /'
cmake --build "${BUILD_DIR}" --target example_segfault \
  -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | sed 's/^/  /'
echo "  build: ok"
echo ""

# ── Section 2: Store layout verification ──────────────────────────────────────
# The POST_BUILD hook writes:
#   <BUILD_DIR>/symbols/crashomon-example-segfault/<build_id>/crashomon-example-segfault.sym

echo "-- Store layout --"

STORE_DIR="${BUILD_DIR}/symbols"
MODULE_NAME="crashomon-example-segfault"
MODULE_DIR="${STORE_DIR}/${MODULE_NAME}"

if [[ -d "${STORE_DIR}" ]]; then
  log_pass "store directory exists: ${STORE_DIR}"
else
  log_fail "store directory missing: ${STORE_DIR}"
  echo ""
  echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
  exit 1
fi

if [[ -d "${MODULE_DIR}" ]]; then
  log_pass "module directory exists: ${MODULE_NAME}/"
else
  log_fail "module directory missing: ${MODULE_NAME}/"
fi

# Find the build-ID subdirectory (there should be exactly one).
BUILD_ID_DIR=""
if [[ -d "${MODULE_DIR}" ]]; then
  BUILD_ID_DIR="$(find "${MODULE_DIR}" -mindepth 1 -maxdepth 1 -type d | head -1 || true)"
fi

if [[ -n "${BUILD_ID_DIR}" ]]; then
  BUILD_ID="$(basename "${BUILD_ID_DIR}")"
  log_pass "build-ID directory exists: ${MODULE_NAME}/${BUILD_ID}/"
else
  log_fail "no build-ID subdirectory found under ${MODULE_NAME}/"
fi

SYM_FILE=""
if [[ -n "${BUILD_ID_DIR}" ]]; then
  SYM_FILE="${BUILD_ID_DIR}/${MODULE_NAME}.sym"
fi

if [[ -n "${SYM_FILE}" && -f "${SYM_FILE}" ]]; then
  log_pass ".sym file exists: ${MODULE_NAME}/${BUILD_ID}/${MODULE_NAME}.sym"
else
  log_fail ".sym file missing"
fi

# Validate the MODULE line format: "MODULE Linux <arch> <build_id> <name>"
# Store the regex in a variable — unquoted $VAR in =~ is the standard bash idiom
# for extended regex matching; quoting the pattern makes + etc. into literals.
if [[ -n "${SYM_FILE}" && -f "${SYM_FILE}" ]]; then
  FIRST_LINE="$(head -1 "${SYM_FILE}")"
  _module_re='^MODULE Linux [^ ]+ [0-9A-Fa-f]+ '
  if [[ "${FIRST_LINE}" =~ $_module_re ]]; then
    log_pass "MODULE line valid: ${FIRST_LINE}"
  else
    log_fail "unexpected MODULE line: ${FIRST_LINE}"
  fi
fi

# crashomon-syms list must recognise the store.
CRASHOMON_SYMS="${PROJECT_ROOT}/_host_toolkit/bin/crashomon-syms"
[[ ! -f "${CRASHOMON_SYMS}" ]] && CRASHOMON_SYMS="${PROJECT_ROOT}/tools/syms/crashomon-syms"
if [[ -f "${CRASHOMON_SYMS}" ]]; then
  LIST_OUTPUT="$("${CRASHOMON_SYMS}" list --store "${STORE_DIR}" 2>&1)"
  LIST_COUNT="$(echo "${LIST_OUTPUT}" | grep -c "crashomon-example-segfault" || true)"
  if [[ "${LIST_COUNT}" -ge 1 ]]; then
    log_pass "crashomon-syms list shows the stored entry"
  else
    log_fail "crashomon-syms list did not show the stored entry"
    echo "  output was: ${LIST_OUTPUT}"
  fi
else
  echo "  SKIP: crashomon-syms not found"
fi

echo ""

# ── Section 3: Crash capture ──────────────────────────────────────────────────
# Start watcherd in the background, crash the example binary via LD_PRELOAD,
# wait for the minidump to appear in pending/.
# Skipped gracefully when watcherd or libcrashomon.so were not built.

echo "-- Crash capture --"

WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
EXAMPLE="${BUILD_DIR}/examples/crashomon-example-segfault"

WORK_DIR="$(mktemp -d)"
WPID=""
cleanup() {
  [[ -n "${WPID}" ]] && { kill "${WPID}" 2>/dev/null || true; wait "${WPID}" 2>/dev/null || true; }
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

DMP_FILE=""

if [[ ! -f "${WATCHERD}" || ! -f "${LIBCRASHOMON}" || ! -f "${EXAMPLE}" ]]; then
  echo "  SKIP: watcherd, libcrashomon.so, or example binary not built"
else
  DB_DIR="${WORK_DIR}/crashdb"
  SOCK="${WORK_DIR}/handler.sock"
  mkdir -p "${DB_DIR}"

  "${WATCHERD}" --db-path="${DB_DIR}" --socket-path="${SOCK}" \
    >"${WORK_DIR}/watcherd.log" 2>&1 &
  WPID=$!

  for _ in 1 2 3 4 5; do [[ -S "${SOCK}" ]] && break; sleep 0.5; done

  if [[ ! -S "${SOCK}" ]]; then
    log_fail "watcherd did not start in time"
    echo "  watcherd log:" >&2; sed 's/^/    /' "${WORK_DIR}/watcherd.log" >&2
  else
    LD_PRELOAD="${LIBCRASHOMON}" \
      CRASHOMON_SOCKET_PATH="${SOCK}" \
      "${EXAMPLE}" 2>/dev/null || true

    for _ in 1 2 3 4 5 6 7 8 9 10; do
      DMP_FILE="$(find "${DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null | head -1 || true)"
      [[ -n "${DMP_FILE}" ]] && break
      sleep 0.5
    done

    kill "${WPID}" 2>/dev/null || true
    wait "${WPID}" 2>/dev/null || true
    WPID=""

    if [[ -n "${DMP_FILE}" && -s "${DMP_FILE}" ]]; then
      log_pass "minidump captured ($(wc -c < "${DMP_FILE}") bytes): $(basename "${DMP_FILE}")"
    else
      log_fail "no minidump produced after crash"
    fi
  fi
fi

echo ""

# ── Section 4: crashomon-analyze against the cmake-generated store ─────────────
# This is the key assertion: the store produced by the cmake POST_BUILD hook must
# be sufficient for full symbolication — no manual crashomon-syms add needed.

echo "-- crashomon-analyze (cmake-generated store) --"

ANALYZE="${PROJECT_ROOT}/tools/analyze/crashomon-analyze"
STACKWALK="$(find "${BUILD_DIR}" -maxdepth 2 -name 'minidump_stackwalk' -type f \
  2>/dev/null | head -1 || true)"

if [[ -z "${DMP_FILE}" ]]; then
  echo "  SKIP: no minidump available (Section 3 skipped or failed)"
elif [[ ! -f "${ANALYZE}" ]]; then
  echo "  SKIP: crashomon-analyze not found"
elif [[ -z "${STACKWALK}" ]]; then
  echo "  SKIP: minidump_stackwalk not built"
else
  ANALYZE_OUTPUT="$("${ANALYZE}" \
    "--store=${STORE_DIR}" \
    "--minidump=${DMP_FILE}" \
    "--stackwalk-binary=${STACKWALK}" 2>&1)" && ANALYZE_EXIT=0 || ANALYZE_EXIT=$?

  if [[ "${ANALYZE_EXIT}" -eq 0 ]]; then
    log_pass "crashomon-analyze exited 0"
  else
    log_fail "crashomon-analyze exited ${ANALYZE_EXIT}"
    echo "  output:" >&2; echo "${ANALYZE_OUTPUT}" | sed 's/^/    /' >&2
  fi

  if echo "${ANALYZE_OUTPUT}" | grep -q "SIGSEGV"; then
    log_pass "output contains SIGSEGV (signal line present)"
  else
    log_fail "output missing SIGSEGV signal line"
  fi

  # Symbolicated function names from examples/segfault.cpp.
  # These appear only when the cmake-generated .sym matched the crashed binary's build ID.
  if echo "${ANALYZE_OUTPUT}" | grep -q "WriteField"; then
    log_pass "output contains 'WriteField' (crash-site frame symbolicated)"
  else
    log_fail "output missing 'WriteField' — store may not have matched the minidump build ID"
    echo "  tip: rebuild to regenerate the store with the current binary's build ID"
  fi

  if echo "${ANALYZE_OUTPUT}" | grep -q "main"; then
    log_pass "output contains 'main' (outer frame symbolicated)"
  else
    log_fail "output missing 'main'"
  fi
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="

if [[ "${FAIL}" -gt 0 ]]; then
  exit 1
fi
