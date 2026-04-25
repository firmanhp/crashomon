# CLAUDE.md — test/

## Overview

The `test/` directory contains integration scripts, stress tests, and demo scripts.
Unit test sources and their CMake configuration live alongside the module they test:

| Component | Sources | CMake |
|---|---|---|
| `lib/` | `lib/test/test_crashomon.cpp`, `test_tags.cpp`, `test_connect_retry.cpp`, `test_terminate_hooks.cpp` | `lib/test/CMakeLists.txt` |
| `daemon/tombstone/` | `daemon/tombstone/test/test_tombstone_formatter.cpp`, `test_minidump_reader.cpp`, `test_register_extract.cpp` | `daemon/tombstone/test/CMakeLists.txt` |
| `daemon/` | `daemon/test/test_disk_manager.cpp`, `test_worker.cpp` | `daemon/test/CMakeLists.txt` |
| `web/` | `web/tests/test_log_parser.py`, `test_analyze.py`, `test_annotations.py` | pytest (no CMake) |

Three test layers:

- **Unit tests** (three GoogleTest binaries): built and run via CTest.
- **dump_syms smoke tests** (`test_dump_syms_smoke.sh`): standalone script, no daemon or CMake needed.
- **Integration / stress tests** (bash): exercise the full live pipeline and require a successful build first.

---

## Unit tests

### Running

`ENABLE_TESTS=ON` adds the `examples/` targets and the three unit test binaries:

```bash
cmake -B build \
    -DENABLE_TESTS=ON \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
cmake --build build
ctest --test-dir build                    # all tests
ctest --test-dir build -R MinidumpReader  # one suite by regex
ctest --test-dir build --output-on-failure
```

### Test binary structure

There are three CTest binaries, each defined in its component's `test/CMakeLists.txt`:

**`crashomon_client_tests`** (`lib/test/CMakeLists.txt`):
| File | Key test patterns |
|---|---|
| `lib/test/test_crashomon.cpp` | `GetEnv()`, `Resolve()` — precedence: explicit config > env var > default |
| `lib/test/test_tags.cpp` | `crashomon_set_tag()`, null guards, overwrite, 255-char truncation |
| `lib/test/test_connect_retry.cpp` | Retry timing, socket-appears-early, and timeout paths |
| `lib/test/test_terminate_hooks.cpp` | `WriteTerminateAnnotation`, `WriteAssertAnnotation` |

**`crashomon_tombstone_tests`** (`daemon/tombstone/test/CMakeLists.txt`):
| File | Key test patterns |
|---|---|
| `daemon/tombstone/test/test_tombstone_formatter.cpp` | Builds `MinidumpInfo` structs by hand; asserts string output |
| `daemon/tombstone/test/test_minidump_reader.cpp` | Error-path tests + fixture-based tests against synthetic `.dmp` files |
| `daemon/tombstone/test/test_register_extract.cpp` | AMD64 and ARM64 GPR extraction with synthetic context structs |

**`crashomon_daemon_tests`** (`daemon/test/CMakeLists.txt`):
| File | Key test patterns |
|---|---|
| `daemon/test/test_disk_manager.cpp` | Real temp dirs/files; size accounting and age-based pruning |
| `daemon/test/test_worker.cpp` | Worker lifecycle and crash handling |

---

## Fixture files

Synthetic fixtures are generated at build time by `daemon/tombstone/test/gen_synthetic_fixtures.cpp`
(built as the `gen_synthetic_fixtures` CMake target, run via a custom command in
`daemon/tombstone/test/CMakeLists.txt`). Outputs land in `build/daemon/tombstone/test/fixtures/`.

`test_minidump_reader.cpp` finds fixtures via:
1. `CRASHOMON_FIXTURES_DIR` environment variable (set automatically by `gtest_discover_tests`)
2. `test/fixtures/` relative to cwd (fallback when run manually from repo root)

The `REQUIRE_FIXTURE(name, path_var)` macro skips gracefully with `GTEST_SKIP()` when
a fixture is absent.

Real fixtures (live minidumps from actual crashes) can be generated with `test/gen_fixtures.sh`.

---

## Shell scripts

### `run_tests.sh`

Builds and runs the full test suite: ctest → pytest → integration_test.sh.

```bash
test/run_tests.sh            # uses ./build
test/run_tests.sh /my/build
```

### `gen_fixtures.sh`

Generates live `.dmp` fixture files by starting watcherd and crashing each example binary.
Outputs to `test/fixtures/`.

```bash
cmake -B build && cmake --build build
test/gen_fixtures.sh                        # writes to test/fixtures/
test/gen_fixtures.sh build test/fixtures    # explicit paths
```

### `integration_test.sh`

Full end-to-end test of the pipeline. Prints `PASS`/`FAIL`/`SKIP` for each check.

```bash
test/integration_test.sh            # uses ./build
test/integration_test.sh /my/build
```

### `test_dump_syms_smoke.sh`

Standalone smoke tests for the `dump_syms` host binary. No daemon required.

```bash
test/test_dump_syms_smoke.sh _dump_syms_build/dump_syms build/examples/crashomon-example-segfault
```

### `test_symbol_store.sh`

Tests the CMake POST_BUILD symbol ingestion hook end-to-end.

```bash
test/test_symbol_store.sh            # uses ./build
```

### `stress_test.sh`

Floods watcherd with concurrent crashes and verifies all minidumps are captured.

```bash
test/stress_test.sh [BUILD_DIR] [-n TOTAL] [-b BATCH] [-p PAUSE] [-t TIMEOUT]
```

---

## Adding new tests

### New unit test

Add to the existing `test_<module>.cpp` in the component's `test/` directory.
If adding a new source file, add its path to the appropriate `CMakeLists.txt`:
- `lib/test/CMakeLists.txt` for client library tests
- `daemon/tombstone/test/CMakeLists.txt` for tombstone tests
- `daemon/test/CMakeLists.txt` for daemon tests

### Important constraints

- No `try`/`catch`/`throw` anywhere — all C++ is compiled with `-fno-exceptions`.
- Module `.cpp` files are compiled directly into the test binary via `target_sources`,
  not via a separate library. Changing internal headers requires only a rebuild.
- When adding a test that includes Crashpad headers directly (as `test_tags.cpp` does),
  add `crashpad_client` to `target_link_libraries` and the `crashpad_interface` SYSTEM-include
  block — otherwise our `-Werror` flags fire on third-party headers.
