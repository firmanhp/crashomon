# CLAUDE.md — test/

## Overview

The test directory contains fixture generation utilities, integration scripts, and end-to-end demo scripts for crashomon. Unit test sources live alongside the module they test:

- `lib/test/test_crashomon.cpp`, `lib/test/test_tags.cpp`, `lib/test/test_connect_retry.cpp`
- `tombstone/test/test_tombstone_formatter.cpp`, `tombstone/test/test_minidump_reader.cpp`, `tombstone/test/test_register_extract.cpp`
- `daemon/test/test_disk_manager.cpp`, `daemon/test/test_worker.cpp`
- `web/tests/test_log_parser.py`, `web/tests/test_analyze.py` (Python, for tools/analyze)

Two test layers:

- **Unit tests** (two GoogleTest binaries, 103 tests total): built and run via CTest.
- **Integration/demo scripts** (bash): exercise the full live pipeline end-to-end and require a successful build first.

---

## Unit tests

### Running

```bash
cmake -B build && cmake --build build
ctest --test-dir build                  # all 103 tests
ctest --test-dir build -R MinidumpReader  # one suite by regex
ctest --test-dir build --output-on-failure
```

All 103 tests pass after a clean build. The `MinidumpReaderTest.*Fixture_*` tests print `SKIP` (not `FAIL`) when fixtures are absent.

### Test binary structure

Test sources are **co-located with the module they test** and referenced by path in `test/CMakeLists.txt`. There are two CTest binaries:

**`crashomon_daemon_tests`** — tombstone and daemon internals:
| File | Module under test | Key test patterns |
|---|---|---|
| `tombstone/test/test_tombstone_formatter.cpp` | `tombstone/tombstone_formatter.cpp` | Builds `MinidumpInfo` structs by hand; asserts string output |
| `tombstone/test/test_minidump_reader.cpp` | `tombstone/minidump_reader.cpp` | Error-path tests + fixture-based tests against real `.dmp` files |
| `tombstone/test/test_register_extract.cpp` | `tombstone/register_extract.cpp` | AMD64 and ARM64 GPR extraction with synthetic context structs |
| `daemon/test/test_disk_manager.cpp` | `daemon/disk_manager.cpp` | Real temp dirs/files; size accounting and age-based pruning |
| `daemon/test/test_worker.cpp` | `daemon/worker.cpp` | Worker lifecycle and crash handling |

**`crashomon_client_tests`** — client library:
| File | Module under test | Key test patterns |
|---|---|---|
| `lib/test/test_crashomon.cpp` | `lib/crashomon_internal.h` — `GetEnv()`, `Resolve()` | `ScopedEnv` RAII guard; precedence: explicit config > env var > default |
| `lib/test/test_tags.cpp` | `lib/crashomon.cpp` — `crashomon_set_tag()`, `crashomon_set_abort_message()` | Installs local `SimpleStringDictionary`; null guards, overwrite, 255-char truncation |
| `lib/test/test_connect_retry.cpp` | `lib/crashomon.cpp` — connect retry loop | Retry timing, socket-appears-early, and timeout paths |

Python tests for `tools/analyze` live at `web/tests/test_log_parser.py` and `web/tests/test_analyze.py`.

### Fixture discovery

`test_minidump_reader.cpp` finds fixtures via:
1. `CRASHOMON_FIXTURES_DIR` environment variable (set automatically by CMake via `gtest_discover_tests PROPERTIES ENVIRONMENT`)
2. `GTEST_BINARY_DIR/fixtures/` (set by the gtest CMake machinery)
3. `test/fixtures/` relative to cwd (fallback when run manually from repo root)

The `REQUIRE_FIXTURE(name, path_var)` macro at the top of each fixture test skips with `GTEST_SKIP()` and a human-readable message pointing to `gen_fixtures.sh` when the file is missing.

---

## Fixture files (`test/fixtures/`)

Fixtures are real Crashpad minidumps (`.dmp`) captured from the example crasher binaries. They are **not** checked into the repository — they must be generated locally.

### What fixtures are

Each `.dmp` is a Breakpad/Crashpad minidump: a structured binary file written by the Crashpad out-of-process handler when a process crashes. It contains:
- Process metadata: PID, process name, timestamp, OS/CPU info
- Thread list with stack frames (raw instruction pointers + register state)
- Module list (loaded shared objects with base addresses and build IDs)
- Exception record: signal number, signal code, fault address

Crashpad captures this without copying the full process heap — it uses `ptrace` to read only registers, stack memory (~128 KB/thread), and `/proc/<pid>/maps`.

### The three fixtures

| Fixture | Source binary | Crash type | Key assertions in tests |
|---|---|---|---|
| `segfault.dmp` | `crashomon-example-segfault` | `SIGSEGV / SEGV_MAPERR` at address `0x4` (null `Record*` field write) | `signal_info` contains `"SIGSEGV"`, crashing thread first, `rip` present in registers |
| `abort.dmp` | `crashomon-example-abort` | `SIGABRT` from `abort(3)` | `signal_info` contains `"SIGABRT"`, crashing thread present |
| `multithread.dmp` | `crashomon-example-multithread` | `SIGSEGV` on a non-main thread | `threads.size() >= 2`, exactly one `is_crashing == true`, non-crashing threads have empty `registers` |

### Generating fixtures

Fixtures require a running `crashomon-watcherd` because `libcrashomon.so` connects to the watcherd socket to deliver the crash via Crashpad. The watcherd writes the `.dmp` to its database `pending/` directory.

```bash
cmake -B build && cmake --build build
test/gen_fixtures.sh                        # writes to test/fixtures/
test/gen_fixtures.sh build test/fixtures    # explicit paths
```

`gen_fixtures.sh` does the following:
1. Creates a temp directory and starts `crashomon-watcherd` with `--db-path` pointing to it.
2. Polls the Unix socket path until the watcherd is ready (up to 2.5 s).
3. Runs each example binary with `LD_PRELOAD=libcrashomon.so` + `CRASHOMON_SOCKET_PATH`.
4. Polls `<db_dir>/pending/` for a `*.dmp` file (up to 5 s per crash).
5. Copies the `.dmp` to `OUTPUT_DIR/<name>.dmp` and removes it from the database.
6. Kills the watcherd and cleans up the temp directory.

**Note on the `new/` vs `pending/` distinction**: Crashpad's `CrashReportDatabase` writes the minidump to `<db_path>/new/<uuid>.dmp`, then atomically renames it to `<db_path>/pending/<uuid>.dmp` once the write is complete. The integration scripts and the watcherd's inotify watch both target `pending/` to avoid reading a partially-written file.

After generating fixtures, re-run CTest and all 101 tests pass (none skip):

```bash
ctest --test-dir build
# 100% tests passed, 0 tests failed out of 101
```

---

## Shell scripts

### `gen_fixtures.sh`

Generates the three `.dmp` fixture files. See [Generating fixtures](#generating-fixtures) above.

### `integration_test.sh`

Full end-to-end test of the pipeline. Prints `PASS`/`FAIL`/`SKIP` for each check and exits non-zero if any check fails.

```bash
test/integration_test.sh            # uses ./build
test/integration_test.sh /my/build
```

What it checks:
1. All built artifacts exist.
2. Each example binary crashes and produces a `.dmp` in `pending/`.
3. `crashomon-analyze` exits 0 and produces non-empty output for each `.dmp`.
4. `crashomon-syms list` succeeds on an empty store.
5. `crashomon-syms add` ingests the segfault binary; `list` shows ≥ 1 entry afterward.

### `demo_crash.sh`

Minimal demo: starts watcherd, crashes the segfault example, waits 2 s for the tombstone to appear on watcherd's stderr.

```bash
test/demo_crash.sh            # uses ./build
test/demo_crash.sh /my/build
```

### `demo_symbolicated.sh`

Full symbolicated crash report demo showing function names and source line numbers. Requires `breakpad_dump_syms` and `minidump_stackwalk` to be built (included in the default CMake build).

```bash
test/demo_symbolicated.sh            # uses ./build
test/demo_symbolicated.sh /my/build
```

What it does:
1. Extracts debug symbols from `crashomon-example-segfault` into a temp symbol store via `crashomon-syms add --store ... --dump-syms breakpad_dump_syms`.
2. Starts watcherd, crashes the example, waits for the `.dmp` to appear in `pending/`.
3. Runs `crashomon-analyze --store=... --minidump=... --stackwalk-binary=...` to produce a fully symbolicated report.

Expected output shows the full 5-level call chain with source locations:
```
 0  (anonymous namespace)::WriteField(...)    [segfault.cpp : 19]
 1  (anonymous namespace)::UpdateRecord(...)  [segfault.cpp : 25]
 2  (anonymous namespace)::ProcessEntry(...)  [segfault.cpp : 30]
 3  (anonymous namespace)::DispatchRecords(…) [segfault.cpp : 41]
 4  (anonymous namespace)::RunPipeline()      [segfault.cpp : 48]
 5  main                                      [segfault.cpp : 55]
```

---

## Adding new tests

### New unit test

Add to the existing `test_<module>.cpp` that lives next to the module being tested (e.g. `daemon/test_disk_manager.cpp`). No CMake changes needed unless adding a new source file.

If adding a new source file, place it in the module's `test/` directory and add its path to `DAEMON_TEST_SOURCES` or `CLIENT_TEST_SOURCES` in `test/CMakeLists.txt`.

### New fixture-based test

Follow the pattern in `test_minidump_reader.cpp`:
1. Use `REQUIRE_FIXTURE("name", path_var)` at the top of the test body to skip gracefully.
2. Add the corresponding crasher to `gen_fixtures.sh` if a new fixture file is needed.

### Important constraints

- No `try`/`catch`/`throw` anywhere — all C++ is compiled with `-fno-exceptions`.
- Module `.cpp` files are compiled directly into the test binary via `target_sources`, not via a separate library. Changing internal headers requires only a rebuild, not a re-link step.
- When adding a test that includes Crashpad headers directly (as `test_tags.cpp` does), add `crashpad_client` to `target_link_libraries` and the `crashpad_interface` SYSTEM-include block in `test/CMakeLists.txt` — otherwise our `-Werror` flags fire on third-party headers.
- The `CRASHOMON_FIXTURES_DIR` env var is injected by CMake's `gtest_discover_tests` — you do not need to set it manually when running via `ctest`.
