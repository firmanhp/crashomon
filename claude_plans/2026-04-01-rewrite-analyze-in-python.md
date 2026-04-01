# Rewrite crashomon-analyze in Python

## Context

`tools/analyze/` is ~940 lines of C++ across 7 files. Its job: CLI symbolication of minidumps and tombstone text. Three of four modes just shell out to `minidump_stackwalk` or `eu-addr2line` via `popen()`. The fourth uses Breakpad's C++ API to format a raw tombstone, but `minidump_stackwalk` produces equivalent output when called with no symbol paths.

The project already has Python infrastructure: `web/analyzer.py` wraps `minidump_stackwalk`, `web/symbol_store.py` manages Breakpad store layout, `tools/syms/crashomon-syms` is a working Python CLI. pytest and ruff are configured.

Rewriting drops the Breakpad/Abseil C++ dependency from the analyze build, cuts code ~30%, and lets the web app import analysis logic directly instead of shelling out.

The `crashomon_tombstone` C++ library is untouched (the daemon needs it).

---

## Mode 4 change

The C++ Mode 4 (`--minidump` only) calls `ReadMinidump()` via Breakpad C++ API. Python version runs `minidump_stackwalk -m <dmp>` (machine-readable, no symbols) and reformats as Android-style tombstone. Register values won't appear in Mode 4 output; the daemon still shows them.

---

## File layout

New Python modules under `web/` (importable), thin CLI script at `tools/analyze/`:

```
web/log_parser.py          # tombstone text parser (port of log_parser.cpp)
web/symbolizer.py          # addr2line symbolizer + formatters (port of symbolizer.cpp)
web/analyze.py             # 4 mode functions + stackwalk -m parser
tools/analyze/crashomon-analyze   # CLI entry point (argparse)
web/tests/test_log_parser.py      # 20 ported unit tests
web/tests/test_analyze.py         # mode + parser tests
```

---

## Steps

### 1. `web/log_parser.py`
Port from `tools/analyze/log_parser.cpp` + `.h`.
- Dataclasses: `ParsedFrame`, `ParsedThread`, `ParsedTombstone` (mirror C++ structs)
- 6 `re.compile()` patterns (same regexes)
- `parse_tombstone(text) -> ParsedTombstone` with same state machine
- Raise `ValueError` on invalid input (replaces `absl::InvalidArgumentError`)

### 2. `web/tests/test_log_parser.py`
Port 20 tests from `test/test_log_parser.cpp`. Same fixture strings, same assertions.

### 3. `web/symbolizer.py`
Port from `tools/analyze/symbolizer.cpp` + `.h`.
- `symbolize_with_addr2line(tombstone, addr2line_binary) -> dict` -- group frames by module, batch `eu-addr2line` calls via `subprocess.run`, parse 2-line-per-addr output
- `format_symbolicated(tombstone, symbols) -> str` -- Android-style tombstone with symbol info
- `parse_stackwalk_machine(text) -> ParsedTombstone` -- parse `minidump_stackwalk -m` pipe-delimited output for Mode 4
- `format_raw_tombstone(tombstone) -> str` -- unsymbolicated Android tombstone from parsed stackwalk

### 4. `web/analyze.py`
Four functions, one per mode:
- `mode_minidump_store(store, dmp, stackwalk) -> str`
- `mode_stdin_store(text, store, addr2line) -> str`
- `mode_sym_file_minidump(sym_file, dmp, stackwalk) -> str` -- reuse `web.symbol_store._parse_module_line()`
- `mode_raw_tombstone(dmp, stackwalk) -> str`

Each returns output text or raises `RuntimeError`.

### 5. `tools/analyze/crashomon-analyze`
Python script with `argparse`. Same flags: `--store=`, `--minidump=`, `--symbols=`, `--stdin`, `--stackwalk-binary=`, `--addr2line-binary=`. `sys.path` fixup to import from `web/`.

### 6. `web/tests/test_analyze.py`
Test `-m` output parsing, temp store creation, mode dispatch with mocked subprocesses.

### 7. Refactor `web/analyzer.py`
`analyze_tombstone_text()`: replace subprocess call to C++ binary with `web.log_parser.parse_tombstone()` + `web.symbolizer.symbolize_with_addr2line()`.

### 8. Remove C++ analyze target
- `CMakeLists.txt` (~line 333): remove `add_subdirectory(tools/analyze)`
- `CMakeLists.txt` (~line 347): remove `crashomon_analyze` from `install(TARGETS ...)`
- `test/CMakeLists.txt` (lines 11, 33): remove `test_log_parser.cpp` and `tools/analyze/log_parser.cpp`
- Delete: `tools/analyze/main.cpp`, `log_parser.{cpp,h}`, `minidump_analyzer.{cpp,h}`, `symbolizer.{cpp,h}`, `tools/analyze/CMakeLists.txt`

### 9. Update shell scripts
- `test/integration_test.sh` line 48: `"${BUILD_DIR}/tools/analyze/crashomon-analyze"` -> `"${PROJECT_ROOT}/tools/analyze/crashomon-analyze"`
- `test/demo_symbolicated.sh` line 22: same

### 10. Update `pyproject.toml`
Extend `testpaths` if needed. No entry point (standalone script like crashomon-syms).

---

## Files to reuse
- `web/symbol_store.py:_parse_module_line()` -- MODULE line parsing
- `web/analyzer.py` -- subprocess pattern reference
- `test/test_log_parser.cpp` -- test fixtures and assertions to port

## Files to modify
- `web/analyzer.py`
- `CMakeLists.txt`
- `test/CMakeLists.txt`
- `test/integration_test.sh` (line 48)
- `test/demo_symbolicated.sh` (line 22)
- `pyproject.toml`

## Files to delete
- `tools/analyze/main.cpp`
- `tools/analyze/log_parser.cpp`, `tools/analyze/log_parser.h`
- `tools/analyze/minidump_analyzer.cpp`, `tools/analyze/minidump_analyzer.h`
- `tools/analyze/symbolizer.cpp`, `tools/analyze/symbolizer.h`
- `tools/analyze/CMakeLists.txt`

---

## Verification
1. `python -m pytest web/tests/` -- all tests pass
2. `ruff check web/ tools/analyze/` -- clean
3. `cmake -B build && cmake --build build && ctest --test-dir build` -- C++ build works, remaining tests pass
4. `tools/analyze/crashomon-analyze --minidump=test/fixtures/segfault.dmp --stackwalk-binary=<path>` -- raw tombstone output
5. `tools/analyze/crashomon-analyze --store=<dir> --minidump=<dmp> --stackwalk-binary=<path>` -- symbolicated output
6. `test/integration_test.sh` and `test/demo_symbolicated.sh` pass
