#!/usr/bin/env bash
# test/demo_crash.sh — demonstrate crashomon end-to-end: capture and tombstone a segfault.
#
# Usage:
#   test/demo_crash.sh [BUILD_DIR]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
BUILD_DIR="$(realpath "${BUILD_DIR}")"

WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
EXAMPLE="${BUILD_DIR}/examples/crashomon-example-segfault"

for f in "${WATCHERD}" "${LIBCRASHOMON}" "${EXAMPLE}"; do
  if [[ ! -f "${f}" ]]; then
    echo "ERROR: not found: ${f}" >&2
    echo "Run: cmake -B build && cmake --build build" >&2
    exit 1
  fi
done

DB_DIR="$(mktemp -d)"
SOCK="${DB_DIR}/handler.sock"
trap 'kill "${WPID}" 2>/dev/null; wait "${WPID}" 2>/dev/null; rm -rf "${DB_DIR}"' EXIT

"${WATCHERD}" --db-path="${DB_DIR}" --socket-path="${SOCK}" &
WPID=$!

for i in 1 2 3 4 5; do [[ -S "${SOCK}" ]] && break; sleep 0.3; done
if [[ ! -S "${SOCK}" ]]; then
  echo "ERROR: watcherd did not start" >&2
  exit 1
fi

echo "=== crashomon-watcherd running — crashing example process ==="
echo ""

LD_PRELOAD="${LIBCRASHOMON}" \
  CRASHOMON_SOCKET_PATH="${SOCK}" \
  "${EXAMPLE}" || true

# Give watcherd time to process the minidump and print the tombstone.
sleep 2
