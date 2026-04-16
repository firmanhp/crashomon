#!/usr/bin/env bash
# test/run_tests.sh — build and run the full crashomon test suite.
#
# Runs in order:
#   1. ctest               — C++ unit tests (client library, daemon, tombstone)
#   2. pytest              — Python unit tests (web/, tools/analyze/)
#   3. integration_test.sh — end-to-end pipeline tests
#
# Usage:
#   test/run_tests.sh [BUILD_DIR]
#
# Exit code: 0 = all suites passed; non-zero = at least one suite failed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${PROJECT_ROOT}/build}"
BUILD_DIR="$(realpath "${BUILD_DIR}")"

SUITES_PASSED=0
SUITES_FAILED=0

suite_pass() { echo "  PASS: $*"; SUITES_PASSED=$((SUITES_PASSED + 1)); }
suite_fail() { echo "  FAIL: $*" >&2; SUITES_FAILED=$((SUITES_FAILED + 1)); }

echo "=== Crashomon Test Runner ==="
echo "Build dir: ${BUILD_DIR}"
echo ""

# ── Build ─────────────────────────────────────────────────────────────────────

echo "-- Build --"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

cmake -B "${BUILD_DIR}" -DENABLE_TESTS=ON --log-level=WARNING 2>&1 | sed 's/^/  /'
cmake --build "${BUILD_DIR}" -j"${NPROC}" 2>&1 | sed 's/^/  /'
echo "  build: ok"

# ── ctest (C++ unit tests) ────────────────────────────────────────────────────

echo ""
echo "-- ctest --"
if ctest --test-dir "${BUILD_DIR}" \
         --output-on-failure \
         --no-header \
         -j"${NPROC}" 2>&1 \
    | sed 's/^/  /'; then
  suite_pass "ctest"
else
  suite_fail "ctest"
fi

# ── Python tests ──────────────────────────────────────────────────────────────

echo ""
echo "-- pytest --"
if uv run python -m pytest "${PROJECT_ROOT}/web/tests/" --tb=short -q 2>&1 \
    | sed 's/^/  /'; then
  suite_pass "pytest"
else
  suite_fail "pytest"
fi

# ── Integration tests ─────────────────────────────────────────────────────────

echo ""
echo "-- integration_test.sh --"
if "${SCRIPT_DIR}/integration_test.sh" "${BUILD_DIR}" 2>&1 | sed 's/^/  /'; then
  suite_pass "integration_test.sh"
else
  suite_fail "integration_test.sh"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "=== Results: ${SUITES_PASSED} suite(s) passed, ${SUITES_FAILED} suite(s) failed ==="
if [[ "${SUITES_FAILED}" -gt 0 ]]; then
  exit 1
fi
