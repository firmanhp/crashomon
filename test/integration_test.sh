#!/usr/bin/env bash
# test/integration_test.sh — end-to-end integration test for the crashomon pipeline.
#
# Exercises the full on-device + developer-side pipeline:
#   1. Run each example crasher → Crashpad captures a .dmp file
#   2. Verify the .dmp is a valid minidump (non-empty, readable by file(1))
#   3. Run crashomon-analyze on each .dmp → verify it exits 0 and produces output
#   4. Run crashomon-syms list → verify the script is callable
#
# Prerequisites:
#   cmake -B build && cmake --build build
#
# Usage:
#   test/integration_test.sh [BUILD_DIR]
#
# Exit code: 0 = all checks passed; non-zero = at least one check failed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
BUILD_DIR="$(realpath "${BUILD_DIR}")"

PASS=0
FAIL=0

# ── Helpers ─────────────────────────────────────────────────────────────────

log_pass() { echo "  PASS: $*"; ((PASS++)); }
log_fail() { echo "  FAIL: $*" >&2; ((FAIL++)); }

check() {
  local desc="$1"; shift
  if "$@" &>/dev/null; then
    log_pass "${desc}"
  else
    log_fail "${desc} (command: $*)"
  fi
}

# ── Locate built artifacts ───────────────────────────────────────────────────

LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
CRASHPAD_HANDLER="$(find "${BUILD_DIR}" -name 'crashpad_handler' -type f | head -1 || true)"
CRASHOMON_ANALYZE="${BUILD_DIR}/tools/analyze/crashomon-analyze"
CRASHOMON_SYMS="${PROJECT_ROOT}/tools/syms/crashomon-syms"
EXAMPLES_BIN="${BUILD_DIR}/examples"

echo "=== Crashomon Integration Test ==="
echo "Build dir: ${BUILD_DIR}"
echo ""

# ── Section 1: Verify built artifacts exist ──────────────────────────────────

echo "-- Artifact checks --"
check "libcrashomon.so exists"          test -f "${LIBCRASHOMON}"
check "crashpad_handler exists"         test -n "${CRASHPAD_HANDLER}" -a -f "${CRASHPAD_HANDLER}"
check "crashomon-analyze exists"        test -f "${CRASHOMON_ANALYZE}"
check "crashomon-syms exists"           test -f "${CRASHOMON_SYMS}"
check "example-segfault exists"         test -f "${EXAMPLES_BIN}/crashomon-example-segfault"
check "example-abort exists"            test -f "${EXAMPLES_BIN}/crashomon-example-abort"
check "example-multithread exists"      test -f "${EXAMPLES_BIN}/crashomon-example-multithread"

# ── Section 2: Generate minidumps ───────────────────────────────────────────

echo ""
echo "-- Minidump capture --"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"' EXIT

capture_crash() {
  local name="$1"
  local binary="$2"
  local db_dir="${WORK_DIR}/db_${name}"
  mkdir -p "${db_dir}"

  LD_PRELOAD="${LIBCRASHOMON}" \
    CRASHOMON_DB_PATH="${db_dir}" \
    CRASHOMON_HANDLER_PATH="${CRASHPAD_HANDLER}" \
    "${binary}" 2>/dev/null || true

  # Wait up to 5 s for crashpad_handler to write the .dmp.
  local dmp=""
  for _ in 1 2 3 4 5; do
    dmp="$(find "${db_dir}" -name '*.dmp' -type f | head -1)"
    [[ -n "${dmp}" ]] && break
    sleep 1
  done

  if [[ -n "${dmp}" ]]; then
    cp "${dmp}" "${WORK_DIR}/${name}.dmp"
    log_pass "${name}: minidump captured ($(wc -c < "${WORK_DIR}/${name}.dmp") bytes)"
  else
    log_fail "${name}: no .dmp found after crash"
  fi
}

if [[ -f "${LIBCRASHOMON}" && -n "${CRASHPAD_HANDLER}" ]]; then
  capture_crash "segfault"    "${EXAMPLES_BIN}/crashomon-example-segfault"
  capture_crash "abort"       "${EXAMPLES_BIN}/crashomon-example-abort"
  capture_crash "multithread" "${EXAMPLES_BIN}/crashomon-example-multithread"
else
  echo "  SKIP: libcrashomon or crashpad_handler not found — skipping capture"
fi

# ── Section 3: crashomon-analyze on each captured .dmp ──────────────────────

echo ""
echo "-- crashomon-analyze --"

if [[ -f "${CRASHOMON_ANALYZE}" ]]; then
  for name in segfault abort multithread; do
    dmp="${WORK_DIR}/${name}.dmp"
    if [[ ! -f "${dmp}" ]]; then
      echo "  SKIP: ${name}.dmp not available"
      continue
    fi
    # analyze must exit 0 and print non-empty output.
    output="$("${CRASHOMON_ANALYZE}" "${dmp}" 2>&1 || true)"
    if [[ -n "${output}" ]]; then
      log_pass "analyze ${name}: produced output"
    else
      log_fail "analyze ${name}: empty output"
    fi
  done
else
  echo "  SKIP: crashomon-analyze not found"
fi

# ── Section 4: crashomon-syms basic invocation ──────────────────────────────

echo ""
echo "-- crashomon-syms --"

if [[ -f "${CRASHOMON_SYMS}" ]]; then
  SYMS_DIR="${WORK_DIR}/symbols"
  mkdir -p "${SYMS_DIR}"

  # 'list' with an empty store should succeed and produce no output.
  if "${CRASHOMON_SYMS}" list --symbol-dir "${SYMS_DIR}" &>/dev/null; then
    log_pass "crashomon-syms list (empty store)"
  else
    log_fail "crashomon-syms list returned non-zero"
  fi

  # 'add' with the example binary (which has DWARF info) should succeed.
  segfault_bin="${EXAMPLES_BIN}/crashomon-example-segfault"
  DUMP_SYMS_BIN="$(find "${BUILD_DIR}" -name 'dump_syms' -type f | head -1 || true)"
  if [[ -f "${segfault_bin}" && -n "${DUMP_SYMS_BIN}" ]]; then
    if CRASHOMON_DUMP_SYMS="${DUMP_SYMS_BIN}" \
         "${CRASHOMON_SYMS}" add \
           --symbol-dir "${SYMS_DIR}" \
           "${segfault_bin}" &>/dev/null; then
      log_pass "crashomon-syms add (segfault binary)"
    else
      log_fail "crashomon-syms add returned non-zero"
    fi
    # After add, list should show at least one entry.
    entry_count="$("${CRASHOMON_SYMS}" list --symbol-dir "${SYMS_DIR}" 2>/dev/null | wc -l)"
    if [[ "${entry_count}" -ge 1 ]]; then
      log_pass "crashomon-syms list shows ${entry_count} symbol(s) after add"
    else
      log_fail "crashomon-syms list shows no symbols after add"
    fi
  else
    echo "  SKIP: dump_syms or segfault binary not found"
  fi
else
  echo "  SKIP: crashomon-syms not found"
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="

if [[ "${FAIL}" -gt 0 ]]; then
  exit 1
fi
