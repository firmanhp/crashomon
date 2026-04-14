#!/usr/bin/env bash
# test/test_run_analyze.sh — tests for the run_analyze CMake target.
#
# Builds the project (ENABLE_TESTS=ON to get synthetic fixtures), then
# exercises the run_analyze target across all four crashomon-analyze modes,
# checking exit codes and non-empty output for each.
#
# Usage:
#   test/test_run_analyze.sh [BUILD_DIR]
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

echo "=== run_analyze target tests ==="
echo "Build dir: ${BUILD_DIR}"
echo ""

# ── Build ────────────────────────────────────────────────────────────────────
# ENABLE_TESTS=ON pulls in the synthetic fixture generator so we have .dmp
# files without needing a live watcherd.

echo "-- Build --"
cmake -B "${BUILD_DIR}" -DENABLE_TESTS=ON --log-level=WARNING 2>&1 | sed 's/^/  /'
cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 4)" 2>&1 | sed 's/^/  /'
echo "  build: ok"
echo ""

FIXTURE_DIR="${BUILD_DIR}/test/fixtures"

# ── Helper ───────────────────────────────────────────────────────────────────
# Reconfigures ANALYZE_ARGS, runs the target, and captures its output.
# Returns the output via stdout; exits non-zero if the target fails.

run_analyze() {
  local args="$1"
  cmake -B "${BUILD_DIR}" -DANALYZE_ARGS="${args}" --log-level=ERROR \
    >/dev/null 2>&1
  cmake --build "${BUILD_DIR}" --target run_analyze 2>/dev/null
}

# ── Mode 1: raw tombstone (--minidump only) ───────────────────────────────────
# minidump_stackwalk -m is called and its machine output is reformatted.
# Verifies --stackwalk-binary is auto-injected correctly.

echo "-- Mode 1: raw tombstone (--minidump only) --"
for fixture in segfault abort multithread; do
  dmp="${FIXTURE_DIR}/${fixture}.dmp"
  if [[ ! -f "${dmp}" ]]; then
    echo "  SKIP: ${fixture}.dmp not found in ${FIXTURE_DIR}"
    continue
  fi
  output="$(run_analyze "--minidump=${dmp}" 2>&1)" && exit_ok=true || exit_ok=false
  if [[ "${exit_ok}" == "true" && -n "${output}" ]]; then
    log_pass "${fixture}: exit 0, non-empty output"
  elif [[ "${exit_ok}" == "false" ]]; then
    log_fail "${fixture}: target exited non-zero"
  else
    log_fail "${fixture}: exit 0 but output was empty"
  fi
done

# ── Mode 2: store + minidump (minidump_stackwalk with symbol path) ────────────
# An empty store is valid — stackwalk still produces unsymbolicated output.

echo ""
echo "-- Mode 2: store + minidump (empty store) --"
SYM_DIR="$(mktemp -d)"
trap 'rm -rf "${SYM_DIR}"' EXIT

dmp="${FIXTURE_DIR}/segfault.dmp"
if [[ -f "${dmp}" ]]; then
  output="$(run_analyze "--store=${SYM_DIR} --minidump=${dmp}" 2>&1)" \
    && exit_ok=true || exit_ok=false
  if [[ "${exit_ok}" == "true" && -n "${output}" ]]; then
    log_pass "segfault + empty store: exit 0, non-empty output"
  elif [[ "${exit_ok}" == "false" ]]; then
    log_fail "segfault + empty store: target exited non-zero"
  else
    log_fail "segfault + empty store: exit 0 but output was empty"
  fi
else
  echo "  SKIP: segfault.dmp not found"
fi

# ── Mode 3: single .sym file + minidump ──────────────────────────────────────
# Build a real .sym from an example binary so there is something to match.
# Skipped if dump_syms or example binaries were not built.

echo ""
echo "-- Mode 3: single .sym file + minidump --"
DUMP_SYMS="$(find "${BUILD_DIR}" -maxdepth 2 -name 'breakpad_dump_syms' -type f \
  2>/dev/null | head -1 || true)"
SEGFAULT_BIN="${BUILD_DIR}/examples/crashomon-example-segfault"

if [[ -n "${DUMP_SYMS}" && -f "${SEGFAULT_BIN}" && -f "${dmp}" ]]; then
  SYM_FILE="$(mktemp --suffix=.sym)"
  trap 'rm -rf "${SYM_DIR}" "${SYM_FILE}"' EXIT
  if "${DUMP_SYMS}" "${SEGFAULT_BIN}" >"${SYM_FILE}" 2>/dev/null; then
    output="$(run_analyze "--symbols=${SYM_FILE} --minidump=${dmp}" 2>&1)" \
      && exit_ok=true || exit_ok=false
    if [[ "${exit_ok}" == "true" && -n "${output}" ]]; then
      log_pass "single .sym file: exit 0, non-empty output"
    elif [[ "${exit_ok}" == "false" ]]; then
      log_fail "single .sym file: target exited non-zero"
    else
      log_fail "single .sym file: exit 0 but output was empty"
    fi
  else
    echo "  SKIP: dump_syms failed on example binary"
  fi
else
  echo "  SKIP: breakpad_dump_syms or example binary not built (need -DENABLE_TESTS=ON)"
fi

# ── Error path: no valid mode args ───────────────────────────────────────────
# The script prints a usage error and exits 1 — the cmake target must also
# propagate that failure so developers see it immediately.

echo ""
echo "-- Error path: no valid mode args --"
if ! run_analyze "" >/dev/null 2>&1; then
  log_pass "empty ANALYZE_ARGS: target exited non-zero (correct)"
else
  log_fail "empty ANALYZE_ARGS: expected non-zero exit, got 0"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
# Restore ANALYZE_ARGS to empty so the cache is clean for subsequent use.
cmake -B "${BUILD_DIR}" -DANALYZE_ARGS="" --log-level=ERROR >/dev/null 2>&1 || true

echo "=== Results: ${PASS} passed, ${FAIL} failed ==="
if [[ "${FAIL}" -gt 0 ]]; then
  exit 1
fi
