#!/usr/bin/env bash
# test/demo_symbolicated.sh — full symbolicated crash demo.
#
# Shows the complete flow:
#   1. Extract debug symbols from the example binary into a symbol store
#   2. Start crashomon-watcherd, crash the example, capture the minidump
#   3. Run crashomon-analyze with the symbol store → function names + line numbers
#
# Usage:
#   test/demo_symbolicated.sh [BUILD_DIR [DUMP_SYMS_PATH]]
#
# dump_syms and minidump_stackwalk are auto-discovered from _dump_syms_build/ or
# co-located with minidump_stackwalk in BUILD_DIR. DUMP_SYMS_PATH can also be
# supplied via the CRASHOMON_DUMP_SYMS_EXECUTABLE environment variable.
# Build the host tools with:
#   cmake -B _dump_syms_build -S cmake/dump_syms_host/ && cmake --build _dump_syms_build

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
BUILD_DIR="$(realpath "${BUILD_DIR}")"

WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
EXAMPLE="${BUILD_DIR}/examples/crashomon-example-segfault"
ANALYZE="${PROJECT_ROOT}/tools/analyze/crashomon-analyze"
SYMS="${PROJECT_ROOT}/tools/syms/crashomon-syms"
# minidump_stackwalk: main build first, then host tools build.
STACKWALK="$(find "${BUILD_DIR}" -maxdepth 2 -name 'minidump_stackwalk' -type f 2>/dev/null | head -1 || true)"
if [[ -z "${STACKWALK}" ]]; then
  STACKWALK="$(find "${PROJECT_ROOT}/_dump_syms_build" -maxdepth 1 -name 'minidump_stackwalk' -type f 2>/dev/null | head -1 || true)"
fi

# dump_syms is a host binary built separately (see cmake/dump_syms_host/).
# Resolution order: CLI arg > env var > co-located with minidump_stackwalk > _dump_syms_build/.
DUMP_SYMS="${2:-${CRASHOMON_DUMP_SYMS_EXECUTABLE:-}}"
if [[ -z "${DUMP_SYMS}" ]] && [[ -n "${STACKWALK}" ]]; then
  CANDIDATE="$(dirname "${STACKWALK}")/dump_syms"
  [[ -f "${CANDIDATE}" ]] && DUMP_SYMS="${CANDIDATE}"
fi
if [[ -z "${DUMP_SYMS}" ]] && [[ -f "${PROJECT_ROOT}/_dump_syms_build/dump_syms" ]]; then
  DUMP_SYMS="${PROJECT_ROOT}/_dump_syms_build/dump_syms"
fi

for f in "${WATCHERD}" "${LIBCRASHOMON}" "${EXAMPLE}" "${ANALYZE}" "${SYMS}"; do
  if [[ ! -f "${f}" ]]; then
    echo "ERROR: not found: ${f}" >&2
    echo "Run: cmake -B build -DENABLE_TESTS=ON && cmake --build build" >&2
    exit 1
  fi
done
if [[ -z "${DUMP_SYMS}" ]]; then
  echo "ERROR: dump_syms not found. Build the host tools with:" >&2
  echo "  cmake -B _dump_syms_build -S cmake/dump_syms_host/ && cmake --build _dump_syms_build" >&2
  exit 1
fi
if [[ ! -f "${DUMP_SYMS}" ]]; then
  echo "ERROR: dump_syms not found at '${DUMP_SYMS}'" >&2
  exit 1
fi
if [[ -z "${STACKWALK}" ]]; then
  echo "ERROR: minidump_stackwalk not found under ${BUILD_DIR}" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d)"
SYM_STORE="${WORK_DIR}/symbols"
DB_DIR="${WORK_DIR}/crashdb"
SOCK="${WORK_DIR}/handler.sock"
mkdir -p "${SYM_STORE}" "${DB_DIR}"

cleanup() {
  kill "${WPID}" 2>/dev/null || true
  wait "${WPID}" 2>/dev/null || true
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# ── Step 1: ingest symbols ───────────────────────────────────────────────────

echo "=== Step 1: extracting debug symbols ==="
CRASHOMON_DUMP_SYMS="${DUMP_SYMS}" \
  "${SYMS}" add --store "${SYM_STORE}" --dump-syms "${DUMP_SYMS}" "${EXAMPLE}"
echo ""
echo "Symbol store contents:"
"${SYMS}" list --store "${SYM_STORE}"
echo ""

# ── Step 2: start watcherd and crash the example ─────────────────────────────

echo "=== Step 2: starting watcherd and crashing example ==="
"${WATCHERD}" --db-path="${DB_DIR}" --socket-path="${SOCK}" 2>/dev/null &
WPID=$!

for i in 1 2 3 4 5; do [[ -S "${SOCK}" ]] && break; sleep 0.3; done
if [[ ! -S "${SOCK}" ]]; then
  echo "ERROR: watcherd did not start" >&2
  exit 1
fi

LD_PRELOAD="${LIBCRASHOMON}" \
  CRASHOMON_SOCKET_PATH="${SOCK}" \
  "${EXAMPLE}" 2>/dev/null || true

# Wait for the minidump to appear in pending/.
DMP=""
for i in 1 2 3 4 5; do
  DMP="$(find "${DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null | head -1 || true)"
  [[ -n "${DMP}" ]] && break
  sleep 1
done

if [[ -z "${DMP}" ]]; then
  echo "ERROR: no minidump produced" >&2
  exit 1
fi
echo "Minidump: ${DMP}"
echo ""

# ── Step 3: symbolicate ───────────────────────────────────────────────────────

echo "=== Step 3: symbolicated crash report ==="
echo ""
"${ANALYZE}" \
  "--store=${SYM_STORE}" \
  "--minidump=${DMP}" \
  "--stackwalk-binary=${STACKWALK}"
