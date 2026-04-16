#!/usr/bin/env bash
# test/test_dump_syms_smoke.sh — smoke tests for the dump_syms host binary.
#
# Usage: test_dump_syms_smoke.sh <dump_syms_path> <debug_elf_path>
#
# Runs standalone — no running daemon, no build system required.
# Pass the path to the pre-built host dump_syms and any debug ELF.

set -uo pipefail

DUMP_SYMS="${1:?Usage: $0 <dump_syms> <debug_elf>}"
DEBUG_ELF="${2:?Usage: $0 <dump_syms> <debug_elf>}"

PASS=0
FAIL=0

pass() { echo "PASS  $1"; ((PASS++)); }
fail() { echo "FAIL  $1"; ((FAIL++)); }

# ── Capture output safely ──────────────────────────────────────────────────────
# Avoid piping dump_syms directly into head/grep: dump_syms may receive SIGPIPE
# when the downstream reader closes early, producing a non-zero exit that
# looks like a real failure. Capture to a variable first, then process in shell.

capture() {
  # Sets _out to stdout of "$DUMP_SYMS $1"; returns dump_syms exit code.
  _out=$("$DUMP_SYMS" "$1")
}

# ── Test 1: exits 0 on a valid debug ELF ─────────────────────────────────────

if capture "$DEBUG_ELF"; then
  pass "exits 0 on valid debug ELF"
else
  fail "exits 0 on valid debug ELF"
fi

# Capture once for the format tests below.
full_output=$("$DUMP_SYMS" "$DEBUG_ELF")
first_line="${full_output%%$'\n'*}"

# ── Test 2: first output line is a MODULE record ──────────────────────────────

if [[ "$first_line" == MODULE\ * ]]; then
  pass "stdout begins with MODULE"
else
  fail "stdout begins with MODULE (got: '${first_line}')"
fi

# ── Test 3: MODULE line has exactly 5 space-separated fields ─────────────────
# Format: MODULE <os> <arch> <build_id> <filename>

read -r -a fields <<<"$first_line"
if [[ ${#fields[@]} -eq 5 ]]; then
  pass "MODULE line has 5 fields"
else
  fail "MODULE line has 5 fields (got ${#fields[@]})"
fi

# ── Test 4: os field is "Linux" ───────────────────────────────────────────────

if [[ "${fields[1]:-}" == "Linux" ]]; then
  pass "MODULE os field is Linux"
else
  fail "MODULE os field is Linux (got '${fields[1]:-}')"
fi

# ── Test 5: build_id is non-empty uppercase hex ───────────────────────────────

build_id="${fields[3]:-}"
if [[ -n "$build_id" && "$build_id" =~ ^[0-9A-F]+$ ]]; then
  pass "build_id is non-empty uppercase hex ('$build_id')"
else
  fail "build_id is non-empty uppercase hex (got '$build_id')"
fi

# ── Test 6: filename field matches the input basename ────────────────────────

expected_name=$(basename "$DEBUG_ELF")
actual_name="${fields[4]:-}"
if [[ "$actual_name" == "$expected_name" ]]; then
  pass "MODULE filename matches input basename"
else
  fail "MODULE filename matches input basename (expected '$expected_name', got '$actual_name')"
fi

# ── Test 7: FUNC records present for a debug ELF ─────────────────────────────

func_count=$(echo "$full_output" | { grep -c '^FUNC ' || true; })
if [[ "$func_count" -gt 0 ]]; then
  pass "FUNC records present in debug ELF output ($func_count found)"
else
  fail "FUNC records present in debug ELF output (none found)"
fi

# ── Test 8: FILE records present for a debug ELF ─────────────────────────────

file_count=$(echo "$full_output" | { grep -c '^FILE ' || true; })
if [[ "$file_count" -gt 0 ]]; then
  pass "FILE records present in debug ELF output ($file_count found)"
else
  fail "FILE records present in debug ELF output (none found)"
fi

# ── Test 9: exits non-zero on a non-existent path ────────────────────────────

if "$DUMP_SYMS" /nonexistent_path_that_cannot_exist; then
  fail "exits non-zero on non-existent path (unexpectedly exited 0)"
else
  pass "exits non-zero on non-existent path"
fi

# ── Test 10: exits non-zero on a non-ELF file ────────────────────────────────

tmpfile=$(mktemp)
echo "this is not an ELF binary" >"$tmpfile"
if "$DUMP_SYMS" "$tmpfile"; then
  fail "exits non-zero on non-ELF file (unexpectedly exited 0)"
else
  pass "exits non-zero on non-ELF file"
fi
rm -f "$tmpfile"

# ── Test 11: stripped binary still produces a MODULE line ────────────────────
# Even without debug info, dump_syms should emit a MODULE record (using the
# GNU build ID from .note.gnu.build-id).

stripped=$(mktemp)
cp "$DEBUG_ELF" "$stripped"
strip "$stripped"
stripped_out=$("$DUMP_SYMS" "$stripped")
stripped_first="${stripped_out%%$'\n'*}"
rm -f "$stripped"
if [[ "$stripped_first" == MODULE\ * ]]; then
  pass "stripped binary still produces MODULE line"
else
  fail "stripped binary still produces MODULE line (got: '${stripped_first}')"
fi

# ── Test 12: dump_syms processes its own binary ───────────────────────────────

self_out=$("$DUMP_SYMS" "$DUMP_SYMS")
self_first="${self_out%%$'\n'*}"
if [[ "$self_first" == MODULE\ * ]]; then
  pass "dump_syms processes its own binary"
else
  fail "dump_syms processes its own binary (got: '${self_first}')"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
