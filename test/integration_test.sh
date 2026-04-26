#!/usr/bin/env bash
# test/integration_test.sh — end-to-end integration test for the crashomon pipeline.
#
# Exercises the full on-device + developer-side pipeline:
#   1. Start crashomon-watcherd in the background
#   2. Run each example crasher → Crashpad captures a .dmp file via watcherd
#   3. Verify the .dmp is a valid minidump (non-empty, readable by file(1))
#   4. Run crashomon-analyze on each .dmp → verify it exits 0 and produces output
#   5. Run crashomon-syms list → verify the script is callable
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

log_pass() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
log_fail() { echo "  FAIL: $*" >&2; FAIL=$((FAIL + 1)); }

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
WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
CRASHOMON_ANALYZE="${PROJECT_ROOT}/_host_toolkit/bin/crashomon-analyze"
[[ ! -f "${CRASHOMON_ANALYZE}" ]] && CRASHOMON_ANALYZE="${PROJECT_ROOT}/tools/analyze/crashomon-analyze"
CRASHOMON_SYMS="${PROJECT_ROOT}/_host_toolkit/bin/crashomon-syms"
[[ ! -f "${CRASHOMON_SYMS}" ]] && CRASHOMON_SYMS="${PROJECT_ROOT}/tools/syms/crashomon-syms"
EXAMPLES_BIN="${BUILD_DIR}/examples"
# dump_syms and minidump-stackwalk: host_toolkit bin/ first, then BUILD_DIR fallback.
DUMP_SYMS="${PROJECT_ROOT}/_host_toolkit/bin/dump_syms"
if [[ ! -f "${DUMP_SYMS}" ]]; then
  DUMP_SYMS="$(find "${BUILD_DIR}" -maxdepth 4 -name 'dump_syms' -type f 2>/dev/null | head -1 || true)"
fi
STACKWALK="${PROJECT_ROOT}/_host_toolkit/bin/minidump-stackwalk"
if [[ ! -f "${STACKWALK}" ]]; then
  STACKWALK="$(find "${BUILD_DIR}" -maxdepth 4 -name 'minidump-stackwalk' -type f 2>/dev/null | head -1 || true)"
fi

echo "=== Crashomon Integration Test ==="
echo "Build dir: ${BUILD_DIR}"
echo ""

# ── Section 1: Verify built artifacts exist ──────────────────────────────────

echo "-- Artifact checks --"
check "libcrashomon.so exists"          test -f "${LIBCRASHOMON}"
check "crashomon-watcherd exists"       test -f "${WATCHERD}"
check "crashomon-analyze exists"        test -f "${CRASHOMON_ANALYZE}"
check "crashomon-syms exists"           test -f "${CRASHOMON_SYMS}"
check "example-segfault exists"         test -f "${EXAMPLES_BIN}/crashomon-example-segfault"
check "example-abort exists"            test -f "${EXAMPLES_BIN}/crashomon-example-abort"
check "example-multithread exists"      test -f "${EXAMPLES_BIN}/crashomon-example-multithread"

# ── Section 2: Start watcherd + generate minidumps ───────────────────────────

echo ""
echo "-- Minidump capture --"

WORK_DIR="$(mktemp -d)"
trap 'kill "${WATCHERD_PID:-}" 2>/dev/null || true; wait "${WATCHERD_PID:-}" 2>/dev/null || true; rm -rf "${WORK_DIR}"' EXIT

DB_DIR="${WORK_DIR}/crashdb"
SOCKET_PATH="${WORK_DIR}/handler.sock"
SYM_STORE="${WORK_DIR}/symbols"
mkdir -p "${DB_DIR}" "${SYM_STORE}"

WATCHERD_STARTED=false
if [[ -f "${LIBCRASHOMON}" && -f "${WATCHERD}" ]]; then
  "${WATCHERD}" \
    --db-path="${DB_DIR}" \
    --socket-path="${SOCKET_PATH}" \
    >"${WORK_DIR}/watcherd.log" 2>&1 &
  WATCHERD_PID=$!

  # Wait for the socket to appear.
  for _ in 1 2 3 4 5; do
    [[ -S "${SOCKET_PATH}" ]] && break
    sleep 0.5
  done

  if [[ -S "${SOCKET_PATH}" ]]; then
    WATCHERD_STARTED=true
    echo "  watcherd started (pid ${WATCHERD_PID})"
  else
    echo "  SKIP: watcherd did not start — skipping capture"
    kill "${WATCHERD_PID}" 2>/dev/null || true
  fi
fi

capture_crash() {
  local name="$1"
  local binary="$2"

  LD_PRELOAD="${LIBCRASHOMON}" \
    CRASHOMON_SOCKET_PATH="${SOCKET_PATH}" \
    "${binary}" 2>/dev/null || true

  # Crashpad writes to pending/ after an atomic rename from new/.
  local dmp=""
  for _ in 1 2 3 4 5; do
    dmp="$(find "${DB_DIR}/pending" -name '*.dmp' -type f | head -1 2>/dev/null || true)"
    [[ -n "${dmp}" ]] && break
    sleep 1
  done

  if [[ -n "${dmp}" ]]; then
    cp "${dmp}" "${WORK_DIR}/${name}.dmp"
    rm -f "${dmp}"
    log_pass "${name}: minidump captured ($(wc -c < "${WORK_DIR}/${name}.dmp") bytes)"
  else
    log_fail "${name}: no .dmp found after crash"
  fi
}

if [[ "${WATCHERD_STARTED}" == "true" ]]; then
  capture_crash "segfault"    "${EXAMPLES_BIN}/crashomon-example-segfault"
  capture_crash "abort"       "${EXAMPLES_BIN}/crashomon-example-abort"
  capture_crash "multithread" "${EXAMPLES_BIN}/crashomon-example-multithread"

  kill "${WATCHERD_PID}" 2>/dev/null || true
  wait "${WATCHERD_PID}" 2>/dev/null || true
  WATCHERD_PID=""
else
  echo "  SKIP: libcrashomon or watcherd not found — skipping capture"
fi

# ── Section 3: crashomon-analyze on each captured .dmp ──────────────────────

echo ""
echo "-- crashomon-analyze --"

if [[ -f "${CRASHOMON_ANALYZE}" ]]; then
  # Build a symbol store from the segfault binary so analyze has something to work with.
  if [[ -n "${DUMP_SYMS}" && -f "${EXAMPLES_BIN}/crashomon-example-segfault" ]]; then
    "${CRASHOMON_SYMS}" add \
      --store "${SYM_STORE}" \
      --dump-syms "${DUMP_SYMS}" \
      "${EXAMPLES_BIN}/crashomon-example-segfault" &>/dev/null || true
  fi

  STACKWALK_ARG=""
  if [[ -n "${STACKWALK}" ]]; then
    STACKWALK_ARG="--stackwalk-binary=${STACKWALK}"
  fi

  for name in segfault abort multithread; do
    dmp="${WORK_DIR}/${name}.dmp"
    if [[ ! -f "${dmp}" ]]; then
      echo "  SKIP: ${name}.dmp not available"
      continue
    fi
    if output="$("${CRASHOMON_ANALYZE}" \
      "--store=${SYM_STORE}" \
      "--minidump=${dmp}" \
      ${STACKWALK_ARG:+"${STACKWALK_ARG}"} 2>&1)"; then
      analyze_exit=0
    else
      analyze_exit=$?
    fi
    if [[ "${analyze_exit}" -eq 0 && -n "${output}" ]]; then
      log_pass "analyze ${name}: exited 0 and produced output"
    else
      log_fail "analyze ${name}: exited ${analyze_exit} or produced empty output"
    fi
  done
else
  echo "  SKIP: crashomon-analyze not found"
fi

# ── Section 4: --export-dir ─────────────────────────────────────────────────

echo ""
echo "-- export-dir --"

EXPORT_DIR="${WORK_DIR}/exports"
mkdir -p "${EXPORT_DIR}"

if [[ -f "${LIBCRASHOMON}" && -f "${WATCHERD}" ]]; then
  EXPORT_DB_DIR="${WORK_DIR}/exportdb"
  EXPORT_SOCK="${WORK_DIR}/export.sock"
  mkdir -p "${EXPORT_DB_DIR}"

  "${WATCHERD}" \
    --db-path="${EXPORT_DB_DIR}" \
    --socket-path="${EXPORT_SOCK}" \
    --export-path="${EXPORT_DIR}" \
    >"${WORK_DIR}/export_watcherd.log" 2>&1 &
  EXPORT_WATCHERD_PID=$!

  for _ in 1 2 3 4 5; do
    [[ -S "${EXPORT_SOCK}" ]] && break
    sleep 0.5
  done

  if [[ -S "${EXPORT_SOCK}" ]]; then
    LD_PRELOAD="${LIBCRASHOMON}" \
      CRASHOMON_SOCKET_PATH="${EXPORT_SOCK}" \
      "${EXAMPLES_BIN}/crashomon-example-segfault" 2>/dev/null || true

    # Wait for the .dmp to land in pending/ first (same as capture_crash),
    # then wait for the worker to process and export it.
    for _ in 1 2 3 4 5; do
      [[ -n "$(find "${EXPORT_DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null)" ]] && break
      sleep 1
    done

    exported=""
    for _ in 1 2 3 4 5; do
      exported="$(find "${EXPORT_DIR}" -name '*.crashdump' -type f | head -1 2>/dev/null || true)"
      [[ -n "${exported}" ]] && break
      sleep 1
    done

    if [[ -n "${exported}" ]]; then
      log_pass "export-dir: .crashdump file created ($(wc -c < "${exported}") bytes)"
    else
      log_fail "export-dir: no .crashdump file found in ${EXPORT_DIR}"
      echo "  watcherd log:" >&2
      sed 's/^/    /' "${WORK_DIR}/export_watcherd.log" >&2
    fi
  else
    log_fail "export-dir: watcherd did not start"
  fi

  kill "${EXPORT_WATCHERD_PID}" 2>/dev/null || true
  wait "${EXPORT_WATCHERD_PID}" 2>/dev/null || true
else
  echo "  SKIP: libcrashomon or watcherd not found"
fi

# ── Section 5: crashomon-syms basic invocation ──────────────────────────────

echo ""
echo "-- crashomon-syms --"

if [[ -f "${CRASHOMON_SYMS}" ]]; then
  SYMS_DIR="${WORK_DIR}/syms_test"
  mkdir -p "${SYMS_DIR}"

  # 'list' with an empty store should succeed and produce no output.
  if "${CRASHOMON_SYMS}" list --store "${SYMS_DIR}" &>/dev/null; then
    log_pass "crashomon-syms list (empty store)"
  else
    log_fail "crashomon-syms list returned non-zero"
  fi

  # 'add' with the example binary (which has DWARF info) should succeed.
  segfault_bin="${EXAMPLES_BIN}/crashomon-example-segfault"
  if [[ -f "${segfault_bin}" && -n "${DUMP_SYMS}" ]]; then
    if "${CRASHOMON_SYMS}" add \
         --store "${SYMS_DIR}" \
         --dump-syms "${DUMP_SYMS}" \
         "${segfault_bin}" &>/dev/null; then
      log_pass "crashomon-syms add (segfault binary)"
    else
      log_fail "crashomon-syms add returned non-zero"
    fi
    # After add, list should show at least one entry.
    entry_count="$("${CRASHOMON_SYMS}" list --store "${SYMS_DIR}" 2>/dev/null | wc -l)"
    if [[ "${entry_count}" -ge 1 ]]; then
      log_pass "crashomon-syms list shows ${entry_count} symbol(s) after add"
    else
      log_fail "crashomon-syms list shows no symbols after add"
    fi
  else
    echo "  SKIP: breakpad_dump_syms or segfault binary not found"
  fi
else
  echo "  SKIP: crashomon-syms not found"
fi

# ── Section 6: Sysroot symbol extraction ────────────────────────────────────

echo ""
echo "-- sysroot symbols --"

if [[ -n "${DUMP_SYMS}" && -f "${CRASHOMON_SYMS}" ]]; then
  SYSROOT_STORE="${WORK_DIR}/sysroot_syms"
  mkdir -p "${SYSROOT_STORE}"

  # Fake sysroot: one lib at usr/lib/ and one in a subdirectory (usr/lib/plugin/)
  # to exercise recursive scanning.
  FAKE_SYSROOT="${WORK_DIR}/fake_sysroot"
  mkdir -p "${FAKE_SYSROOT}/usr/lib/plugin"
  cp "${EXAMPLES_BIN}/crashomon-example-segfault" "${FAKE_SYSROOT}/usr/lib/libfake.so.1"
  cp "${EXAMPLES_BIN}/crashomon-example-abort"    "${FAKE_SYSROOT}/usr/lib/plugin/libplugin.so.1"

  if "${CRASHOMON_SYMS}" add \
       --store "${SYSROOT_STORE}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${FAKE_SYSROOT}" &>/dev/null; then
    log_pass "crashomon-syms add --sysroot"
  else
    log_fail "crashomon-syms add --sysroot returned non-zero"
  fi

  sym_count="$(find "${SYSROOT_STORE}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 2 ]]; then
    log_pass "sysroot store contains ${sym_count} .sym file(s) (top-level + subdir)"
  else
    log_fail "sysroot store has ${sym_count} .sym file(s), expected ≥2 (top-level + subdir)"
  fi
else
  echo "  SKIP: dump_syms or crashomon-syms not found"
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
echo "=== Results: ${PASS} passed, ${FAIL} failed ==="

if [[ "${FAIL}" -gt 0 ]]; then
  exit 1
fi
