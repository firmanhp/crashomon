#!/usr/bin/env bash
# test/test_syms.sh — comprehensive tests for crashomon-syms.
#
# Covers all subcommands and every sysroot scan path:
#   1. Prerequisites (gcc, dump_syms, crashomon-syms)
#   2. Fixture compilation from test/fixtures/syms/*.c
#   3. list (empty store)
#   4. add (single binary) — store layout + MODULE line validation
#   5. add (multiple binaries) + list
#   6. add --recursive
#   7. prune (three versions → keep 1)
#   8. --sysroot usr/lib/          (flat)
#   9. --sysroot usr/lib/plugin/   (recursive subdir)
#  10. --sysroot usr/lib64/
#  11. --sysroot lib/
#  12. --sysroot lib64/
#  13. --sysroot merged-usr symlink (lib/ → usr/lib/) — dedup check
#  14. --sysroot non-ELF .so files — skipped silently
#  15. --sysroot empty sysroot — exits non-zero
#  16. Error paths (no binaries, non-existent binary, prune --keep 0, bad store)
#
# Compiled fixtures come from test/fixtures/syms/ (committed C source files).
#
# Usage:
#   test/test_syms.sh [--dump-syms <path>] [--syms-tool <path>]
#
# Defaults:
#   --dump-syms   _host_toolkit/bin/dump_syms (falls back to PATH)
#   --syms-tool   _host_toolkit/bin/crashomon-syms → tools/syms/crashomon-syms
#
# Exit code: 0 = all checks passed; non-zero = at least one check failed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/fixtures/syms"

# ── Argument parsing ──────────────────────────────────────────────────────────

DUMP_SYMS=""
SYMS_TOOL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dump-syms)  DUMP_SYMS="$2"; shift 2 ;;
    --syms-tool)  SYMS_TOOL="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

if [[ -z "${DUMP_SYMS}" ]]; then
  if [[ -f "${PROJECT_ROOT}/_host_toolkit/bin/dump_syms" ]]; then
    DUMP_SYMS="${PROJECT_ROOT}/_host_toolkit/bin/dump_syms"
  elif command -v dump_syms &>/dev/null; then
    DUMP_SYMS="$(command -v dump_syms)"
  fi
fi

if [[ -z "${SYMS_TOOL}" ]]; then
  if [[ -f "${PROJECT_ROOT}/_host_toolkit/bin/crashomon-syms" ]]; then
    SYMS_TOOL="${PROJECT_ROOT}/_host_toolkit/bin/crashomon-syms"
  elif [[ -f "${PROJECT_ROOT}/tools/syms/crashomon-syms" ]]; then
    SYMS_TOOL="${PROJECT_ROOT}/tools/syms/crashomon-syms"
  fi
fi

# ── Helpers ───────────────────────────────────────────────────────────────────

PASS=0
FAIL=0
SKIP=0

log_pass() { echo "  PASS: $*"; PASS=$((PASS + 1)); }
log_fail() { echo "  FAIL: $*" >&2; FAIL=$((FAIL + 1)); }
log_skip() { echo "  SKIP: $*"; SKIP=$((SKIP + 1)); }

compile_so() {
  local src="$1" out="$2" extra="${3:-}"
  # shellcheck disable=SC2086
  gcc -shared -fPIC -g -O0 -Wl,--build-id=sha1 ${extra} -o "${out}" "${src}" 2>/dev/null
}

echo "=== crashomon-syms test suite ==="
echo "dump_syms:      ${DUMP_SYMS:-<not found>}"
echo "crashomon-syms: ${SYMS_TOOL:-<not found>}"
echo ""

# ── Section 1: Prerequisites ─────────────────────────────────────────────────

echo "-- Prerequisites --"

HAVE_GCC=false
if command -v gcc &>/dev/null; then
  HAVE_GCC=true
  log_pass "gcc available"
else
  log_skip "gcc not found — compiled-fixture tests will be skipped"
fi

HAVE_DUMP_SYMS=false
if [[ -n "${DUMP_SYMS}" && ( -f "${DUMP_SYMS}" || -x "${DUMP_SYMS}" ) ]]; then
  HAVE_DUMP_SYMS=true
  log_pass "dump_syms: ${DUMP_SYMS}"
else
  log_skip "dump_syms not found — symbol extraction tests will be skipped"
fi

if [[ -n "${SYMS_TOOL}" && -f "${SYMS_TOOL}" ]]; then
  log_pass "crashomon-syms: ${SYMS_TOOL}"
else
  log_fail "crashomon-syms not found — cannot continue"
  echo ""
  echo "=== Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="
  exit 1
fi

echo ""

# ── Work directory ────────────────────────────────────────────────────────────

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# ── Section 2: Compile fixture libraries ─────────────────────────────────────

echo "-- Compile fixtures --"

HAVE_FIXTURES=false
if [[ "${HAVE_GCC}" == "true" ]]; then
  mkdir -p "${WORK}/lib"
  ok=true
  compile_so "${FIXTURES_DIR}/libfoo.c"            "${WORK}/lib/libfoo.so.1"    || ok=false
  compile_so "${FIXTURES_DIR}/libbar.c"            "${WORK}/lib/libbar.so.1"    || ok=false
  compile_so "${FIXTURES_DIR}/plugin/libplugin.c"  "${WORK}/lib/libplugin.so.1" || ok=false

  if [[ "${ok}" == "true" ]]; then
    HAVE_FIXTURES=true
    log_pass "compiled libfoo.so.1, libbar.so.1, libplugin.so.1"
  else
    log_fail "one or more fixture compilations failed"
  fi
else
  log_skip "gcc not available"
fi

echo ""

# ── Section 3: list (empty store) ────────────────────────────────────────────

echo "-- list (empty store) --"

EMPTY_STORE="${WORK}/empty_store"
mkdir -p "${EMPTY_STORE}"

if "${SYMS_TOOL}" list --store "${EMPTY_STORE}" &>/dev/null; then
  log_pass "list exits 0 on empty store"
else
  log_fail "list exited non-zero on empty store"
fi

list_out="$("${SYMS_TOOL}" list --store "${EMPTY_STORE}" 2>/dev/null)"
if echo "${list_out}" | grep -q "(no symbols stored)"; then
  log_pass "list reports '(no symbols stored)'"
else
  log_fail "list output missing '(no symbols stored)': ${list_out}"
fi

echo ""

# ── Section 4: add (single binary) + store layout ────────────────────────────

echo "-- add (single binary) + store layout --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  STORE1="${WORK}/store1"
  if "${SYMS_TOOL}" add \
       --store "${STORE1}" \
       --dump-syms "${DUMP_SYMS}" \
       "${WORK}/lib/libfoo.so.1" &>/dev/null; then
    log_pass "add single binary exits 0"
  else
    log_fail "add single binary exited non-zero"
  fi

  # Verify Breakpad layout: <store>/<module_name>/<build_id>/<module_name>.sym
  module_dir="${STORE1}/libfoo.so.1"
  if [[ -d "${module_dir}" ]]; then
    log_pass "module directory exists: libfoo.so.1/"

    build_id_dir="$(find "${module_dir}" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1)"
    if [[ -n "${build_id_dir}" ]]; then
      build_id="$(basename "${build_id_dir}")"
      log_pass "build-ID directory exists: libfoo.so.1/${build_id}/"

      sym_file="${build_id_dir}/libfoo.so.1.sym"
      if [[ -f "${sym_file}" ]]; then
        log_pass ".sym file exists: libfoo.so.1/${build_id}/libfoo.so.1.sym"

        # Validate MODULE line: MODULE Linux <arch> <build_id_hex> libfoo.so.1
        first_line="$(head -1 "${sym_file}")"
        read -r -a mod_parts <<<"${first_line}"
        _mod_re='^MODULE Linux [^ ]+ [0-9A-F]+ libfoo'
        if [[ "${first_line}" =~ ${_mod_re} ]]; then
          log_pass "MODULE line valid: ${first_line}"
        else
          log_fail "MODULE line format unexpected: ${first_line}"
        fi

        # Build ID in directory name must match MODULE line field [3]
        if [[ "${build_id}" == "${mod_parts[3]:-}" ]]; then
          log_pass "directory build_id matches MODULE line build_id (${build_id})"
        else
          log_fail "directory build_id '${build_id}' != MODULE build_id '${mod_parts[3]:-}'"
        fi
      else
        log_fail ".sym file missing: expected ${sym_file}"
      fi
    else
      log_fail "no build-ID subdirectory under libfoo.so.1/"
    fi
  else
    log_fail "module directory missing: ${module_dir}"
  fi
else
  log_skip "add single binary (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 5: add (multiple binaries) + list ────────────────────────────────

echo "-- add (multiple binaries) + list --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  STORE2="${WORK}/store2"
  if "${SYMS_TOOL}" add \
       --store "${STORE2}" \
       --dump-syms "${DUMP_SYMS}" \
       "${WORK}/lib/libfoo.so.1" \
       "${WORK}/lib/libbar.so.1" &>/dev/null; then
    log_pass "add two binaries exits 0"
  else
    log_fail "add two binaries exited non-zero"
  fi

  entry_count="$("${SYMS_TOOL}" list --store "${STORE2}" 2>/dev/null | grep -c 'KB)' || true)"
  if [[ "${entry_count}" -eq 2 ]]; then
    log_pass "list shows 2 entries after adding 2 binaries"
  else
    log_fail "list shows ${entry_count} entry/entries, expected 2"
  fi

  # Both module names should appear in list output.
  list2="$("${SYMS_TOOL}" list --store "${STORE2}" 2>/dev/null)"
  if echo "${list2}" | grep -q "libfoo" && echo "${list2}" | grep -q "libbar"; then
    log_pass "list names both libfoo and libbar"
  else
    log_fail "list missing expected module names: ${list2}"
  fi
else
  log_skip "add multiple binaries (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 6: add --recursive ───────────────────────────────────────────────

echo "-- add --recursive --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  STORE3="${WORK}/store3"
  RECUR_DIR="${WORK}/recursive_bins"
  mkdir -p "${RECUR_DIR}/deep"
  cp "${WORK}/lib/libfoo.so.1" "${RECUR_DIR}/libfoo.so.1"
  cp "${WORK}/lib/libbar.so.1" "${RECUR_DIR}/deep/libbar.so.1"

  if "${SYMS_TOOL}" add \
       --store "${STORE3}" \
       --dump-syms "${DUMP_SYMS}" \
       --recursive "${RECUR_DIR}" &>/dev/null; then
    log_pass "--recursive exits 0"
  else
    log_fail "--recursive exited non-zero"
  fi

  sym_count="$(find "${STORE3}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 2 ]]; then
    log_pass "--recursive stored ${sym_count} symbol(s) (top-level + subdir)"
  else
    log_fail "--recursive stored ${sym_count} symbol(s), expected ≥2"
  fi
else
  log_skip "--recursive (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 7: prune ─────────────────────────────────────────────────────────

echo "-- prune --"

if [[ "${HAVE_GCC}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  PRUNE_STORE="${WORK}/prune_store"

  # Compile three versions of libfoo with different VERSION values so that each
  # binary has distinct content → distinct SHA1 build IDs.  All share the same
  # filename so dump_syms reports the same module name; all three end up as
  # sibling build-ID directories under PRUNE_STORE/libfoo.so.1/.
  for ver in 1 2 3; do
    vdir="${WORK}/prune_v${ver}"
    mkdir -p "${vdir}"
    compile_so "${FIXTURES_DIR}/libfoo.c" "${vdir}/libfoo.so.1" "-DVERSION=${ver}" || true
    "${SYMS_TOOL}" add \
      --store "${PRUNE_STORE}" \
      --dump-syms "${DUMP_SYMS}" \
      "${vdir}/libfoo.so.1" &>/dev/null || true
    sleep 0.05  # ensure distinct mtime on each .sym for prune ordering
  done

  before_count="$(find "${PRUNE_STORE}" -name '*.sym' | wc -l)"

  if "${SYMS_TOOL}" prune --store "${PRUNE_STORE}" --keep 1 &>/dev/null; then
    log_pass "prune exits 0"
  else
    log_fail "prune exited non-zero"
  fi

  after_count="$(find "${PRUNE_STORE}" -name '*.sym' | wc -l)"
  if [[ "${after_count}" -eq 1 ]]; then
    log_pass "prune --keep 1: ${before_count} → ${after_count} (oldest removed)"
  else
    log_fail "prune --keep 1: expected 1 after prune, got ${after_count}"
  fi
else
  log_skip "prune (gcc or dump_syms not available)"
fi

echo ""

# ── Section 8: --sysroot usr/lib/ (flat) ─────────────────────────────────────

echo "-- sysroot: usr/lib/ (flat) --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_flat"
  mkdir -p "${SROOT}/usr/lib"
  cp "${WORK}/lib/libfoo.so.1" "${SROOT}/usr/lib/libfoo.so.1"
  cp "${WORK}/lib/libbar.so.1" "${SROOT}/usr/lib/libbar.so.1"

  STORE_F="${WORK}/store_flat"
  if "${SYMS_TOOL}" add \
       --store "${STORE_F}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot flat exits 0"
  else
    log_fail "--sysroot flat exited non-zero"
  fi

  sym_count="$(find "${STORE_F}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 2 ]]; then
    log_pass "--sysroot flat: ${sym_count} .sym file(s)"
  else
    log_fail "--sysroot flat: ${sym_count} .sym file(s), expected ≥2"
  fi
else
  log_skip "sysroot flat (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 9: --sysroot recursive subdir (usr/lib/plugin/) ──────────────────

echo "-- sysroot: recursive subdir (usr/lib/plugin/) --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_subdir"
  mkdir -p "${SROOT}/usr/lib/plugin"
  cp "${WORK}/lib/libfoo.so.1"    "${SROOT}/usr/lib/libfoo.so.1"
  cp "${WORK}/lib/libplugin.so.1" "${SROOT}/usr/lib/plugin/libplugin.so.1"

  STORE_S="${WORK}/store_subdir"
  if "${SYMS_TOOL}" add \
       --store "${STORE_S}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot recursive exits 0"
  else
    log_fail "--sysroot recursive exited non-zero"
  fi

  sym_count="$(find "${STORE_S}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 2 ]]; then
    log_pass "--sysroot recursive: ${sym_count} .sym file(s) (usr/lib/ + usr/lib/plugin/)"
  else
    log_fail "--sysroot recursive: ${sym_count} .sym file(s), expected ≥2"
  fi
else
  log_skip "sysroot recursive subdir (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 10: --sysroot usr/lib64/ ─────────────────────────────────────────

echo "-- sysroot: usr/lib64/ --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_lib64"
  mkdir -p "${SROOT}/usr/lib64"
  cp "${WORK}/lib/libfoo.so.1" "${SROOT}/usr/lib64/libfoo.so.1"

  STORE_L="${WORK}/store_lib64"
  if "${SYMS_TOOL}" add \
       --store "${STORE_L}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot usr/lib64/ exits 0"
  else
    log_fail "--sysroot usr/lib64/ exited non-zero"
  fi

  sym_count="$(find "${STORE_L}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 1 ]]; then
    log_pass "--sysroot usr/lib64/: ${sym_count} .sym file(s)"
  else
    log_fail "--sysroot usr/lib64/: no .sym file found"
  fi
else
  log_skip "sysroot usr/lib64/ (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 11: --sysroot lib/ ───────────────────────────────────────────────

echo "-- sysroot: lib/ --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_lib"
  mkdir -p "${SROOT}/lib"
  cp "${WORK}/lib/libfoo.so.1" "${SROOT}/lib/libfoo.so.1"

  STORE_LB="${WORK}/store_lib"
  if "${SYMS_TOOL}" add \
       --store "${STORE_LB}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot lib/ exits 0"
  else
    log_fail "--sysroot lib/ exited non-zero"
  fi

  sym_count="$(find "${STORE_LB}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 1 ]]; then
    log_pass "--sysroot lib/: ${sym_count} .sym file(s)"
  else
    log_fail "--sysroot lib/: no .sym file found"
  fi
else
  log_skip "sysroot lib/ (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 12: --sysroot lib64/ ─────────────────────────────────────────────

echo "-- sysroot: lib64/ --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_lib64b"
  mkdir -p "${SROOT}/lib64"
  cp "${WORK}/lib/libfoo.so.1" "${SROOT}/lib64/libfoo.so.1"

  STORE_L6="${WORK}/store_lib64b"
  if "${SYMS_TOOL}" add \
       --store "${STORE_L6}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot lib64/ exits 0"
  else
    log_fail "--sysroot lib64/ exited non-zero"
  fi

  sym_count="$(find "${STORE_L6}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -ge 1 ]]; then
    log_pass "--sysroot lib64/: ${sym_count} .sym file(s)"
  else
    log_fail "--sysroot lib64/: no .sym file found"
  fi
else
  log_skip "sysroot lib64/ (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 13: --sysroot merged-usr symlink dedup ───────────────────────────
# lib/ is a symlink to usr/lib/ (common in merged-usr systems).
# crashomon-syms resolves symlinks and deduplicates by real path,
# so the library should be stored exactly once.

echo "-- sysroot: merged-usr symlink dedup --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_merged"
  mkdir -p "${SROOT}/usr/lib"
  cp "${WORK}/lib/libfoo.so.1" "${SROOT}/usr/lib/libfoo.so.1"
  ln -s usr/lib "${SROOT}/lib"  # merged-usr: lib/ → usr/lib/

  STORE_M="${WORK}/store_merged"
  if "${SYMS_TOOL}" add \
       --store "${STORE_M}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot merged-usr exits 0"
  else
    log_fail "--sysroot merged-usr exited non-zero"
  fi

  sym_count="$(find "${STORE_M}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -eq 1 ]]; then
    log_pass "--sysroot merged-usr: exactly 1 .sym (symlink dedup working)"
  else
    log_fail "--sysroot merged-usr: ${sym_count} .sym file(s), expected exactly 1"
  fi
else
  log_skip "sysroot merged-usr dedup (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 14: --sysroot non-ELF .so silently skipped ───────────────────────

echo "-- sysroot: non-ELF .so skip --"

if [[ "${HAVE_FIXTURES}" == "true" && "${HAVE_DUMP_SYMS}" == "true" ]]; then
  SROOT="${WORK}/sysroot_nonelf"
  mkdir -p "${SROOT}/usr/lib"
  cp "${WORK}/lib/libfoo.so.1" "${SROOT}/usr/lib/libfoo.so.1"
  printf "not an ELF binary"   > "${SROOT}/usr/lib/libscript.so.0"  # fake .so

  STORE_NE="${WORK}/store_nonelf"
  if "${SYMS_TOOL}" add \
       --store "${STORE_NE}" \
       --dump-syms "${DUMP_SYMS}" \
       --sysroot "${SROOT}" &>/dev/null; then
    log_pass "--sysroot with non-ELF .so exits 0"
  else
    log_fail "--sysroot with non-ELF .so exited non-zero"
  fi

  sym_count="$(find "${STORE_NE}" -name '*.sym' | wc -l)"
  if [[ "${sym_count}" -eq 1 ]]; then
    log_pass "non-ELF .so skipped: only real ELF stored (1 .sym)"
  else
    log_fail "non-ELF .so skip: expected 1 .sym, got ${sym_count}"
  fi
else
  log_skip "sysroot non-ELF skip (fixtures or dump_syms not available)"
fi

echo ""

# ── Section 15: --sysroot empty sysroot → non-zero exit ──────────────────────

echo "-- sysroot: empty sysroot --"

SROOT_EMPTY="${WORK}/sysroot_empty"
mkdir -p "${SROOT_EMPTY}/usr/lib"  # directory exists but has no .so files

STORE_EMPTY="${WORK}/store_empty"
if "${SYMS_TOOL}" add \
     --store "${STORE_EMPTY}" \
     --dump-syms "${DUMP_SYMS:-dump_syms}" \
     --sysroot "${SROOT_EMPTY}" &>/dev/null; then
  log_fail "--sysroot empty: should exit non-zero (no libraries found)"
else
  log_pass "--sysroot empty: exits non-zero as expected"
fi

echo ""

# ── Section 16: error paths ───────────────────────────────────────────────────

echo "-- error paths --"

STORE_ERR="${WORK}/store_err"
mkdir -p "${STORE_ERR}"

# add with no binaries and no --recursive/--sysroot
if "${SYMS_TOOL}" add --store "${STORE_ERR}" --dump-syms "${DUMP_SYMS:-dump_syms}" &>/dev/null; then
  log_fail "add with no input should exit non-zero"
else
  log_pass "add with no input: exits non-zero"
fi

# add with a non-existent binary
if "${SYMS_TOOL}" add \
     --store "${STORE_ERR}" \
     --dump-syms "${DUMP_SYMS:-dump_syms}" \
     /nonexistent_binary_crashomon_test &>/dev/null; then
  log_fail "add non-existent binary should exit non-zero"
else
  log_pass "add non-existent binary: exits non-zero"
fi

# prune --keep 0 should fail
if "${SYMS_TOOL}" prune --store "${STORE_ERR}" --keep 0 &>/dev/null; then
  log_fail "prune --keep 0 should exit non-zero"
else
  log_pass "prune --keep 0: exits non-zero"
fi

# list with a file path instead of a directory
NOT_A_DIR="${WORK}/not_a_dir"
touch "${NOT_A_DIR}"
if "${SYMS_TOOL}" list --store "${NOT_A_DIR}" &>/dev/null; then
  log_fail "list with non-directory store should exit non-zero"
else
  log_pass "list with non-directory store: exits non-zero"
fi

echo ""

# ── Summary ───────────────────────────────────────────────────────────────────

echo "=== Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped ==="

if [[ "${FAIL}" -gt 0 ]]; then
  exit 1
fi
