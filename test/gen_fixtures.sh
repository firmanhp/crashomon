#!/usr/bin/env bash
# test/gen_fixtures.sh — generate .dmp fixture files from the example crasher binaries.
#
# Starts crashomon-watcherd in the background to host the Crashpad handler.
# Runs each example with LD_PRELOAD=libcrashomon.so so that Crashpad captures a
# real minidump on crash.  The resulting .dmp files are written to OUTPUT_DIR
# (default: test/fixtures/) and are used by test_minidump_reader.cpp.
#
# Usage:
#   test/gen_fixtures.sh [BUILD_DIR [OUTPUT_DIR]]
#
# Example (from repo root):
#   cmake -B build && cmake --build build
#   test/gen_fixtures.sh build test/fixtures

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
OUTPUT_DIR="${2:-${SCRIPT_DIR}/fixtures}"

BUILD_DIR="$(realpath "${BUILD_DIR}")"
mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(realpath "${OUTPUT_DIR}")"

# ── Locate built artifacts ──────────────────────────────────────────────────

LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
if [[ ! -f "${LIBCRASHOMON}" ]]; then
  echo "ERROR: libcrashomon.so not found: ${LIBCRASHOMON}" >&2
  exit 1
fi

WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
if [[ ! -f "${WATCHERD}" ]]; then
  echo "ERROR: crashomon-watcherd not found: ${WATCHERD}" >&2
  exit 1
fi

EXAMPLES_BIN="${BUILD_DIR}/examples"

echo "libcrashomon:     ${LIBCRASHOMON}"
echo "crashomon-watcherd: ${WATCHERD}"
echo "examples bin:     ${EXAMPLES_BIN}"
echo "output dir:       ${OUTPUT_DIR}"

# ── Start watcherd ──────────────────────────────────────────────────────────

DB_DIR="$(mktemp -d)"
SOCKET_PATH="${DB_DIR}/handler.sock"
# shellcheck disable=SC2064
trap "rm -rf '${DB_DIR}'" EXIT

WATCHERD_LOG="${DB_DIR}/watcherd.log"
"${WATCHERD}" \
  --db-path="${DB_DIR}" \
  --socket-path="${SOCKET_PATH}" \
  >"${WATCHERD_LOG}" 2>&1 &
WATCHERD_PID=$!
# shellcheck disable=SC2064
trap "kill '${WATCHERD_PID}' 2>/dev/null || true; rm -rf '${DB_DIR}'" EXIT

# Wait for the socket to appear (watcherd is ready when it binds).
for _ in 1 2 3 4 5; do
  [[ -S "${SOCKET_PATH}" ]] && break
  sleep 0.5
done
if [[ ! -S "${SOCKET_PATH}" ]]; then
  echo "ERROR: watcherd did not start (socket not found: ${SOCKET_PATH})" >&2
  cat "${WATCHERD_LOG}" >&2
  exit 1
fi

echo "watcherd started (pid ${WATCHERD_PID}, socket ${SOCKET_PATH})"

# ── Helper: run one crasher and capture its .dmp ──────────────────────────

run_example() {
  local fixture_name="$1"   # e.g. "segfault"
  local binary="$2"         # full path to the example binary

  if [[ ! -f "${binary}" ]]; then
    echo "SKIP ${fixture_name}: binary not found (${binary})"
    return 0
  fi

  echo ""
  echo "=== ${fixture_name} ==="

  # The crasher exits non-zero (signal); that is expected — ignore the error.
  LD_PRELOAD="${LIBCRASHOMON}" \
    CRASHOMON_SOCKET_PATH="${SOCKET_PATH}" \
    "${binary}" 2>/dev/null || true

  # Crashpad writes the minidump asynchronously; give it up to 5 s to appear.
  local dmp_file=""
  for _ in 1 2 3 4 5; do
    dmp_file="$(find "${DB_DIR}/new" -name '*.dmp' -type f | head -1 2>/dev/null || true)"
    [[ -n "${dmp_file}" ]] && break
    sleep 1
  done

  if [[ -z "${dmp_file}" ]]; then
    echo "WARNING: no .dmp produced for ${fixture_name} — skipping" >&2
    return 0
  fi

  local dest="${OUTPUT_DIR}/${fixture_name}.dmp"
  cp "${dmp_file}" "${dest}"
  rm -f "${dmp_file}"
  printf "Saved %s (%d bytes)\n" "${dest}" "$(wc -c < "${dest}")"
}

# ── Generate fixtures ──────────────────────────────────────────────────────

run_example "segfault"    "${EXAMPLES_BIN}/crashomon-example-segfault"
run_example "abort"       "${EXAMPLES_BIN}/crashomon-example-abort"
run_example "multithread" "${EXAMPLES_BIN}/crashomon-example-multithread"

echo ""
echo "Done.  Fixtures in ${OUTPUT_DIR}:"
ls -lh "${OUTPUT_DIR}"/*.dmp 2>/dev/null || echo "  (none generated)"
