#!/usr/bin/env bash
# generate_traces.sh — build and run all stackwalking-case examples, then
# symbolicate each minidump with --show-trust so every frame shows its
# recovery method (context / cfi / frame_pointer / scan).
#
# For each numbered case directory (01_*, 02_*, ...):
#   1. Build the crasher binary with `make`
#   2. Run `dump_syms` on the binary to produce a .sym file
#   3. Crash the binary under libcrashomon.so (Crashpad captures a minidump)
#   4. Symbolicate with `crashomon-analyze --show-trust`
#
# Prerequisites — build the project first (from the repo root):
#
#   cmake -B _dump_syms_build -S cmake/dump_syms_host/
#   cmake --build _dump_syms_build -j$(nproc)
#
#   cmake -B build \
#         -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" \
#         -DCRASHOMON_BUILD_ANALYZE=ON
#   cmake --build build -j$(nproc)
#
# Usage (from the repo root):
#   examples/stackwalking_cases/generate_traces.sh [OPTIONS]
#
# Options:
#   --build-dir DIR      Project build directory           (default: ./build)
#   --dump-syms  PATH    Path to dump_syms binary          (default: ./_dump_syms_build/dump_syms)
#   --stackwalk  PATH    Path to minidump-stackwalk binary (auto-discovered from build-dir)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ── Defaults ─────────────────────────────────────────────────────────────────

BUILD_DIR="${ROOT}/build"
DUMP_SYMS="${ROOT}/_dump_syms_build/dump_syms"
STACKWALK=""

# ── Argument parsing ──────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir=*)  BUILD_DIR="${1#*=}" ;;
        --build-dir)    BUILD_DIR="$2"; shift ;;
        --dump-syms=*)  DUMP_SYMS="${1#*=}" ;;
        --dump-syms)    DUMP_SYMS="$2"; shift ;;
        --stackwalk=*)  STACKWALK="${1#*=}" ;;
        --stackwalk)    STACKWALK="$2"; shift ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1 ;;
    esac
    shift
done

BUILD_DIR="$(realpath "${BUILD_DIR}")"

# ── Auto-discover minidump-stackwalk if not supplied ──────────────────────────

if [[ -z "${STACKWALK}" ]]; then
    # rust-minidump produces minidump-stackwalk (hyphen); older installs may use
    # minidump_stackwalk (underscore).  Check the canonical Cargo output path first.
    for candidate in \
        "${BUILD_DIR}/rust_minidump_target/release/minidump-stackwalk" \
        "${BUILD_DIR}/rust_minidump_target/release/minidump_stackwalk"; do
        if [[ -f "${candidate}" ]]; then
            STACKWALK="${candidate}"
            break
        fi
    done
fi
if [[ -z "${STACKWALK}" ]]; then
    STACKWALK="$(find "${BUILD_DIR}" -maxdepth 3 \
        \( -name 'minidump-stackwalk' -o -name 'minidump_stackwalk' \) \
        -type f 2>/dev/null | head -1 || true)"
fi

# ── Locate required artifacts ─────────────────────────────────────────────────

LIBCRASHOMON="${BUILD_DIR}/lib/libcrashomon.so"
WATCHERD="${BUILD_DIR}/daemon/crashomon-watcherd"
ANALYZE="${ROOT}/tools/analyze/crashomon-analyze"

echo "=== Prerequisites ==="
ok=true
for pair in \
    "libcrashomon.so:${LIBCRASHOMON}" \
    "crashomon-watcherd:${WATCHERD}" \
    "dump_syms:${DUMP_SYMS}" \
    "crashomon-analyze:${ANALYZE}"; do
    name="${pair%%:*}"
    path="${pair#*:}"
    if [[ -f "${path}" ]]; then
        printf "  %-24s %s\n" "${name}" "${path}"
    else
        printf "  %-24s NOT FOUND: %s\n" "${name}" "${path}" >&2
        ok=false
    fi
done

if [[ -z "${STACKWALK}" ]]; then
    echo "  minidump-stackwalk       NOT FOUND" >&2
    echo "" >&2
    echo "  Build with: cmake -B build -DCRASHOMON_BUILD_ANALYZE=ON ..." >&2
    ok=false
else
    printf "  %-24s %s\n" "minidump-stackwalk" "${STACKWALK}"
fi

if [[ "${ok}" != "true" ]]; then
    echo "" >&2
    echo "Run: cmake -B _dump_syms_build -S cmake/dump_syms_host/ && cmake --build _dump_syms_build" >&2
    echo "     cmake -B build -DCRASHOMON_DUMP_SYMS_EXECUTABLE=\$(pwd)/_dump_syms_build/dump_syms \\" >&2
    echo "           -DCRASHOMON_BUILD_ANALYZE=ON && cmake --build build" >&2
    exit 1
fi

# ── Temporary workspace ───────────────────────────────────────────────────────

WORK="$(mktemp -d)"
DB_DIR="${WORK}/crashdb"
SOCK="${WORK}/handler.sock"
mkdir -p "${DB_DIR}"

cleanup() {
    kill "${WATCHERD_PID:-0}" 2>/dev/null || true
    wait "${WATCHERD_PID:-0}" 2>/dev/null || true
    rm -rf "${WORK}"
}
trap cleanup EXIT

# ── Start watcherd ────────────────────────────────────────────────────────────

echo ""
echo "=== Starting crashomon-watcherd ==="
"${WATCHERD}" \
    --db-path="${DB_DIR}" \
    --socket-path="${SOCK}" \
    >"${WORK}/watcherd.log" 2>&1 &
WATCHERD_PID=$!

for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ -S "${SOCK}" ]] && break
    sleep 0.3
done
if [[ ! -S "${SOCK}" ]]; then
    echo "ERROR: watcherd did not start (socket not found)" >&2
    cat "${WORK}/watcherd.log" >&2
    exit 1
fi
echo "watcherd started (pid ${WATCHERD_PID})"

# ── Per-case runner ───────────────────────────────────────────────────────────

run_binary() {
    local binary="$1"
    local case_name="$2"
    local bin_name
    bin_name="$(basename "${binary}")"

    printf "\n"
    printf -- '-%.0s' {1..50}; printf '\n'
    printf "  Binary: %s\n" "${bin_name}"
    printf -- '-%.0s' {1..50}; printf '\n'

    # Extract symbols
    printf "\n[dump_syms]\n"
    local sym_file="${WORK}/${case_name}_${bin_name}.sym"
    "${DUMP_SYMS}" "${binary}" >"${sym_file}" 2>/dev/null
    local func_count cfi_count
    func_count=$(grep -c '^FUNC' "${sym_file}" 2>/dev/null || echo 0)
    cfi_count=$(grep -c '^STACK CFI' "${sym_file}" 2>/dev/null || echo 0)
    printf "  FUNC records: %s\n" "${func_count}"
    printf "  STACK CFI records: %s\n" "${cfi_count}"

    # Crash and capture minidump
    printf "\n[crash]\n"
    LD_PRELOAD="${LIBCRASHOMON}" \
        CRASHOMON_SOCKET_PATH="${SOCK}" \
        "${binary}" >/dev/null 2>&1 || true

    local dmp=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        dmp="$(find "${DB_DIR}/pending" -name '*.dmp' -type f 2>/dev/null | head -1 || true)"
        [[ -n "${dmp}" ]] && break
        sleep 0.5
    done

    if [[ -z "${dmp}" ]]; then
        echo "WARNING: no minidump captured — skipping symbolication" >&2
        return 0
    fi
    printf "  minidump: %d bytes\n" "$(wc -c < "${dmp}")"

    # Symbolicate with trust annotations
    printf "\n[crashomon-analyze --show-trust]\n\n"
    "${ANALYZE}" \
        "--symbols=${sym_file}" \
        "--minidump=${dmp}" \
        "--stackwalk-binary=${STACKWALK}" \
        --show-trust 2>/dev/null

    rm -f "${dmp}"
}

run_case() {
    local case_dir="$1"
    local case_name
    case_name="$(basename "${case_dir}")"

    printf "\n"
    printf '=%.0s' {1..66}; printf '\n'
    printf "  Case: %s\n" "${case_name}"
    printf '=%.0s' {1..66}; printf '\n'

    # Build
    printf "\n[build]\n"
    make -C "${case_dir}" >/dev/null

    # Collect binaries: prefer crasher* glob; fall back to any executable file.
    local binaries=()
    while IFS= read -r -d '' f; do
        binaries+=("$f")
    done < <(find "${case_dir}" -maxdepth 1 -name 'crasher*' -type f \
                  -perm /111 -print0 2>/dev/null | sort -z)

    if [[ ${#binaries[@]} -eq 0 ]]; then
        echo "ERROR: no crasher* binary found in ${case_dir}" >&2
        return 1
    fi

    for binary in "${binaries[@]}"; do
        run_binary "${binary}" "${case_name}"
    done
}

# ── Run all cases ─────────────────────────────────────────────────────────────

for case_dir in "${SCRIPT_DIR}"/0*/; do
    [[ -d "${case_dir}" ]] || continue
    [[ -f "${case_dir}/Makefile" ]] || continue
    run_case "${case_dir%/}"
done

printf "\n"
printf '=%.0s' {1..66}; printf '\n'
echo "  Done."
printf '=%.0s' {1..66}; printf '\n\n'
