# Self-sufficient tombstones: fast copy-paste symbolication

## Context

Embedded developers integrating crashomon via `FetchContent` keep unstripped ELF debug binaries on their **host** machine, separate from the stripped binaries deployed to the **target**. When a crash happens, pulling the minidump off the target and running `crashomon-analyze --minidump` is the slow path. The tombstone text, however, is already visible in the target's journalctl logs in real time — if it carried enough information, a developer could copy-paste it into `crashomon-analyze`, point at their host-side debug directory, and get symbolicated stacks without ever touching the minidump.

Today the tombstone is **not** self-sufficient:

1. Frame lines print only `module_path` + `module_offset`. There is no build ID, so an analyzer cannot verify that a host-side debug ELF matches the binary that crashed — for an embedded developer rebuilding constantly, picking the wrong debug binary gives silently wrong stacks.
2. The existing `--store --stdin` mode calls `eu-addr2line` on whatever path appears in the frame line, which is the **target** path. On the host that path either does not exist or resolves to a different binary.
3. `tools/analyze/log_parser.py` anchors frame regex matches at start-of-line; journalctl's `Mon DD HH:MM:SS host unit[pid]: ` prefix breaks parsing outright.

Intended outcome: tombstone lines carry per-frame build IDs, and a new analyzer mode takes a directory of host-side debug ELFs, indexes them by build ID, and resolves frames — robust to pasted log-line prefixes.

---

## Tombstone format change

Append `(BuildId: <hex>)` to each backtrace frame line (matches the Android bionic tombstone convention):

```
    #00 pc 0x00000000000011a0  /usr/bin/my_service (BuildId: 9c1e3a...e2f0)
    #01 pc 0x0000000000023b09  /lib/libc.so.6 (BuildId: 7a4f0b...11d3)
    #02 pc 0x00000000badf00d0  ???
```

`???` frames and frames whose module has no build ID remain unchanged. This is backward-compatible: existing parsers that don't know about `BuildId` ignore the trailing token.

Build IDs are already captured in `MinidumpInfo::modules[*].build_id` by `CollectModules()` in `tombstone/minidump_reader.cpp:46-63` — no reader-side change needed.

---

## File layout

Changes are scoped to the formatter, the Python analyzer, and their tests:

```
tombstone/tombstone_formatter.cpp       # emit (BuildId: ...) per frame
tools/analyze/log_parser.py             # prefix stripper + BuildId capture
tools/analyze/symbolizer.py             # build-ID index + build-ID-based symbolization
tools/analyze/analyze.py                # new mode_stdin_debug_dir
tools/analyze/crashomon-analyze         # new --debug-dir CLI arg
test/test_tombstone_formatter.cpp       # assert BuildId suffix
web/tests/test_log_parser.py            # prefix + BuildId test cases
```

---

## Steps

### 1. `tombstone/tombstone_formatter.cpp`

`AppendFrames()` currently takes only `const std::vector<FrameInfo>&`. Give it access to `MinidumpInfo::modules` so it can look up `build_id` by matching `frame.module_path` against `ModuleInfo::path`.

Shape: inside `FormatTombstone()`, build a `std::unordered_map<std::string_view, std::string_view>` of `path → build_id` once, pass it to `AppendFrames`. For each mapped frame, append `" (BuildId: " + build_id + ")"` after the existing `" " + module_path` segment. Skip the suffix when `build_id` is empty.

No changes to `tombstone/tombstone_formatter.h` — public surface is unaffected.

### 2. `test/test_tombstone_formatter.cpp`

Extend the hand-built `MinidumpInfo` fixtures (`MakeInfo()` and friends) to populate `info.modules` with entries whose `path` matches the frames' `module_path` and whose `build_id` is a known hex string. Assert:
- mapped frames contain `(BuildId: <hex>)`
- `???` frames do not contain `BuildId`
- frames whose module has empty `build_id` do not contain `BuildId`

### 3. `tools/analyze/log_parser.py` — generic log-prefix stripper

Add a small preprocessor `_strip_log_prefix(line) -> str` that runs on every line before regex matching. It contains one compiled regex per known tombstone token:

- `*** *** ***` (separator)
- `pid:` (header)
- `signal ` (signal line)
- `timestamp:` (timestamp)
- `backtrace:`
- `--- --- ---` (other-thread separator)
- `\s*#\d+\s+pc ` (frame)
- `minidump saved to:` (footer)

For each line, scan for the earliest match among these tokens and slice the line to start there. If no token matches, return the line unchanged (blank lines and register-state noise stay out of the parser's way as they do today). The existing `.search()` calls then match cleanly regardless of any leading `Apr 14 10:15:30 host crashomon-watcherd[1234]: ` or ISO-8601 prefix.

### 4. `tools/analyze/log_parser.py` — BuildId capture

- Extend `_FRAME_RE` so its trailing group optionally captures `(BuildId: <hex>)`. Keep the existing symbolicated trailing group `(symbol) [src:line]` working so the formatter's output still round-trips through the parser.
- Add `build_id: str = ""` to `ParsedFrame`.
- Populate `ParsedFrame.build_id` (lowercase) when the capture group matches.

### 5. `tools/analyze/symbolizer.py` — build-ID index

Add two functions:

**`build_build_id_index(debug_dir: Path) -> dict[str, Path]`**
- Walk `debug_dir` recursively, keep files whose first 4 bytes are `\x7fELF` (same pattern as `tools/syms/crashomon-syms:_iter_elfs`, lines 86–99).
- For each ELF, extract the build ID by parsing `.note.gnu.build-id` in pure Python: read the ELF header, iterate program headers looking for `PT_NOTE` (or fall back to section headers scanning for `.note.gnu.build-id`), walk notes looking for `name == "GNU\0"` and `type == NT_GNU_BUILD_ID (3)`, emit the descriptor bytes as lowercase hex. ~40 lines of `struct.unpack`. No shell-out to `readelf` / `file` — keeps the tool self-contained and avoids parsing human-readable output.
- Return `{build_id_hex_lower: elf_path}`. On duplicate build IDs, keep the first and print a warning to stderr.

**`symbolize_with_build_id_index(tombstone, index, addr2line) -> SymbolTable`**
- Mirror of the existing `symbolize_with_addr2line` (lines 76–108): group frames by the **resolved local ELF path** (from `index[frame.build_id]`), batch one `eu-addr2line -e <local_elf> -f -s <offsets>` call per local ELF, parse the 2-lines-per-address output.
- Frames whose `build_id` is empty or not in `index` map to `SymbolInfo(function="??")` — the existing formatter already renders these as-is.

### 6. `tools/analyze/analyze.py`

Add one new mode function:

```python
def mode_stdin_debug_dir(text: str, debug_dir: str, addr2line: str) -> str:
    tombstone = parse_tombstone(text)
    index = build_build_id_index(Path(debug_dir))
    symbols = symbolize_with_build_id_index(tombstone, index, addr2line)
    return format_symbolicated(tombstone, symbols)
```

### 7. `tools/analyze/crashomon-analyze`

- Add `--debug-dir=<dir>` argparse argument.
- Dispatch: when `--debug-dir` and `--stdin` are both set, call `mode_stdin_debug_dir`. Takes precedence over `--store --stdin` if both are given.
- Update the module docstring to list the new mode alongside the existing four.

### 8. `web/tests/test_log_parser.py`

Extend with:
- A journalctl-prefixed tombstone (`Apr 14 10:15:30 host crashomon-watcherd[1234]: ...`) that round-trips to the same `ParsedTombstone` as an unprefixed sample.
- A syslog-style ISO-8601-prefixed sample.
- A frame line with a `(BuildId: ...)` suffix — assert `ParsedFrame.build_id` is populated.
- A mix: some frames with BuildId, some without, some `???`.

---

## Files to reuse
- `tombstone/minidump_reader.cpp:46-63` — `CollectModules()` already populates `build_id`
- `tools/syms/crashomon-syms:_iter_elfs` (lines 86–99) — ELF magic-byte walker pattern
- `tools/analyze/symbolizer.py:symbolize_with_addr2line` (lines 76–108) — batching pattern for `eu-addr2line` calls
- Android bionic tombstone BuildId convention

## Files to modify
- `tombstone/tombstone_formatter.cpp`
- `test/test_tombstone_formatter.cpp`
- `tools/analyze/log_parser.py`
- `tools/analyze/symbolizer.py`
- `tools/analyze/analyze.py`
- `tools/analyze/crashomon-analyze`
- `web/tests/test_log_parser.py`

## What not to change
- `MinidumpInfo` / `FrameInfo` / `ModuleInfo` structs — build IDs already captured.
- `tombstone/tombstone_formatter.h` — public surface stays stable.
- Existing Mode 1/2/3/4 dispatch paths — left intact for backward compatibility.
- The Breakpad `.sym` store path (`crashomon-syms`) — out of scope; this adds a parallel path, not a replacement.
- `test/test_log_parser.cpp` — already stale (references a non-existent `tools/analyze/log_parser.h`); flag separately to the user, not as part of this change.
- No new dependency on `eu-readelf` / `readelf` / `file` — ELF note parsing done in pure Python.

---

## Verification

1. `cmake -B build && cmake --build build && ctest --test-dir build` — extended `test_tombstone_formatter` asserts the `(BuildId: ...)` suffix on mapped frames and its absence on `???` frames.
2. `python -m pytest web/tests/test_log_parser.py` — new test cases cover journalctl/syslog prefixes and BuildId capture.
3. `ruff check tools/analyze/` — clean.
4. **Fixture round-trip.** Regenerate `test/fixtures/segfault.dmp` via `test/gen_fixtures.sh`, run the daemon against it or call `FormatTombstone` directly in a unit test, and confirm the `(BuildId: <hex>)` emitted matches `eu-readelf -n ./build/examples/crashomon-example-segfault | grep 'Build ID'`.
5. **End-to-end paste flow** (the user story):
   ```bash
   journalctl -u crashomon-watcherd --since "1 minute ago" > /tmp/tomb.txt
   tools/analyze/crashomon-analyze --debug-dir=build/examples --stdin < /tmp/tomb.txt
   ```
   Expected: fully symbolicated stack for `crashomon-example-segfault` showing the same 5-level chain as `test/demo_symbolicated.sh` (`WriteField → UpdateRecord → ProcessEntry → DispatchRecords → RunPipeline → main`) with source file + line numbers — **without** touching the `.dmp` file or `minidump_stackwalk`.
6. **Negative path.** Repeat step 5 with `--debug-dir` pointing at an empty directory: same tombstone shape, all frames resolve to `??`, no errors.
7. **Log-prefix tolerance.** Hand-prepend `Apr 14 10:15:30 host crashomon-watcherd[1234]: ` to every line of the captured tombstone, pipe through the analyzer, confirm identical symbolicated output to the un-prefixed run.
