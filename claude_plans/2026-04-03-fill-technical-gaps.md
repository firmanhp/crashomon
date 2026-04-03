# Plan: Fill Technical Gaps from Assessment

## Context

The `crashomon-assessment.md` identifies six technical gaps. This plan addresses each one, ordered by CI/test reliability impact first, then feature completeness.

---

## Phase 1: CI/Test Reliability

### 1.1 — Wire Python tests into ctest (Gap 2, quickest win)

**Problem**: 5 Python test files (~101 tests) in `web/tests/` are invisible to `ctest`.

**Approach**: Add a single `add_test()` call that invokes `pytest`.

**Files to modify**:
- `CMakeLists.txt` (root, after `add_subdirectory(test)` block ~line 336):
  - `find_package(Python3 COMPONENTS Interpreter QUIET)`
  - `add_test(NAME python_tests COMMAND ${Python3_EXECUTABLE} -m pytest --tb=short -q WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})`
  - Label `python` so `ctest -L python` works
  - Gate on `Python3_FOUND`

**Verification**: `ctest --test-dir build` shows `python_tests`; `ctest -L python --test-dir build` runs only Python tests.

---

### 1.2 — ARM64 register extraction tests (Gap 3)

**Problem**: `ExtractARM64Regs()` in `tombstone/minidump_reader.cpp:63-82` is in an anonymous namespace with zero test coverage.

**Approach**: Extract register functions to a testable internal header/source, then add unit tests.

**Files to create**:
- `tombstone/register_extract.h` — internal header (not public), declares:
  ```cpp
  namespace crashomon {
  std::vector<std::pair<std::string, uint64_t>> ExtractAMD64Regs(const MDRawContextAMD64&);
  std::vector<std::pair<std::string, uint64_t>> ExtractARM64Regs(const MDRawContextARM64&);
  }
  ```
- `tombstone/register_extract.cpp` — function bodies moved from `minidump_reader.cpp` anonymous namespace
- `test/test_register_extract.cpp` — new test file

**Files to modify**:
- `tombstone/minidump_reader.cpp` — remove `ExtractAMD64Regs`/`ExtractARM64Regs` from anonymous namespace, `#include "tombstone/register_extract.h"`, call `crashomon::ExtractAMD64Regs`/`crashomon::ExtractARM64Regs`
- `tombstone/CMakeLists.txt` — add `register_extract.cpp` to `crashomon_tombstone` sources
- `test/CMakeLists.txt` — add `test_register_extract.cpp` to `TEST_SOURCES`

**Test cases** (~6 tests):
- `ARM64_AllRegistersPresent` — 33 entries returned
- `ARM64_CorrectNames` — x0-x28, fp, lr, sp, pc
- `ARM64_CorrectValues` — known values map correctly
- `ARM64_ZeroContext` — all zeros
- `AMD64_AllRegistersPresent` — 17 entries
- `AMD64_CorrectValues` — known values

**Verification**: `ctest -R RegisterExtract --test-dir build`

---

### 1.3 — Synthetic minidump fixtures (Gap 1)

**Problem**: 18 fixture tests in `test/test_minidump_reader.cpp` skip when `.dmp` files are absent. Generation requires running the full daemon.

**Approach**: Write a C++ generator (`test/gen_synthetic_fixtures.cpp`) that creates valid `.dmp` files at build time using Breakpad's `synth_minidump.h` test utilities. Run it as a `add_custom_command` so fixtures appear in `${CMAKE_CURRENT_BINARY_DIR}/fixtures/` before test discovery.

**Fixtures to generate**:
- `segfault.dmp` — 1 thread, SIGSEGV (signal 11), AMD64 context with known registers, 1 module
- `abort.dmp` — 1 thread, SIGABRT (signal 6), AMD64 context, 1 module
- `multithread.dmp` — 3+ threads, SIGSEGV on one thread, AMD64 contexts, 1 module

**Files to create**:
- `test/gen_synthetic_fixtures.cpp` — uses `google_breakpad::SynthMinidump::Dump`, `Thread`, `Context`, `Module`, `Exception`, `String` etc.

**Files to modify**:
- `cmake/breakpad.cmake` — add a `breakpad_synth_minidump` static library target compiling `synth_minidump.cc` + `test_assembler.cc` from the fetched Breakpad source (test-only)
- `test/CMakeLists.txt`:
  - Add `gen_synthetic_fixtures` executable linked to `breakpad_synth_minidump`
  - Add `add_custom_command(OUTPUT fixtures/segfault.dmp ... COMMAND gen_synthetic_fixtures DEPENDS gen_synthetic_fixtures)`
  - Add `add_custom_target(synthetic_fixtures DEPENDS fixtures/segfault.dmp ...)`
  - Make `crashomon_tests` depend on `synthetic_fixtures`
  - Change `CRASHOMON_FIXTURES_DIR` from `${CMAKE_CURRENT_SOURCE_DIR}/fixtures` to `${CMAKE_CURRENT_BINARY_DIR}/fixtures`

**Key risk**: Breakpad's `synth_minidump.h` may not support AMD64 contexts directly. If so, fall back to writing raw minidump binary format manually (populate `MDRawHeader`, `MDRawDirectory`, `MDRawThreadList`, `MDRawContextAMD64`, etc. as byte arrays). The format is well-documented in `minidump_format.h`.

**Verification**: `ctest --test-dir build` runs all 18 fixture tests (0 skips). `ctest --test-dir build -R MinidumpReader` shows all passing.

---

## Phase 2: Developer Experience

### 2.1 — Implement benchmarks (Gap 4)

**Problem**: `bench/` has no source files; `BENCH_SOURCES` is empty.

**Files to create**:
- `bench/bench_tombstone_formatter.cpp`:
  - `BM_FormatTombstone_SingleThread` — 1 thread, 10 frames, 17 registers
  - `BM_FormatTombstone_MultiThread` — parameterized: 1/4/8/16 threads
  - `BM_FormatTombstone_DeepStack` — parameterized: 10/50/100/500 frames
  - Build `MinidumpInfo` structs inline (same pattern as `test_tombstone_formatter.cpp`)

- `bench/bench_minidump_reader.cpp`:
  - `BM_ReadMinidump` — parse a synthetic `.dmp` file repeatedly
  - Write a temp fixture in benchmark setup, or use `CRASHOMON_FIXTURES_DIR` env var

**Files to modify**:
- `bench/CMakeLists.txt`:
  - Uncomment the two source files
  - Add link deps: `crashomon_tombstone`, `breakpad_processor`, `absl::status`, `absl::statusor`

**Verification**: `cmake -B build -DENABLE_BENCHMARKS=ON && cmake --build build && ./build/bench/crashomon_bench`

---

## Phase 3: Feature Gaps

### 3.1 — Crash grouping, dedup, and trends (Gap 5)

**Problem**: Every crash is a flat row. No signature-based grouping, no time-series view.

**Approach**: Add crash signature column + grouped query + dashboard enhancements.

**Step A — Signature computation**:
- `web/models.py`:
  - Add `signature TEXT NOT NULL DEFAULT ''` column (ALTER TABLE migration on first access)
  - `compute_signature(process, signal, report)` — SHA-256 hash of `process + signal + top 3 frame module+offset` from report text, truncated to 16 hex chars
  - `insert_crash()` computes and stores signature

**Step B — Grouped queries**:
- `web/models.py`:
  - `get_crash_groups(db_path)` — GROUP BY signature, returns count/first_seen/last_seen/sample_crash_id
  - `get_daily_counts(db_path, days=14)` — crash count per day for trend

**Step C — UI**:
- `web/app.py` — add `/groups` route, enhance dashboard context
- `web/templates/dashboard.html` — add crash groups table + daily trend table
- `web/templates/groups.html` (new) — full grouped view

**Tests to add**:
- `web/tests/test_models.py`: `test_compute_signature`, `test_get_crash_groups`, `test_get_daily_counts`
- `web/tests/test_routes.py`: `test_groups_page`

**Verification**: Run `pytest web/tests/` (and now also `ctest -L python`).

---

### 3.2 — Optional web UI authentication (Gap 6)

**Problem**: No auth. Fine for lab, not for networked deployment.

**Approach**: Opt-in via env vars. When unset, no auth (preserves current behavior).

- `CRASHOMON_API_KEY` — protects `/api/*` routes (checked via `X-API-Key` header)
- `CRASHOMON_AUTH_USER` / `CRASHOMON_AUTH_PASS` — HTTP Basic Auth for web routes

**Files to create**:
- `web/auth.py` — `init_auth(app)` registers `@app.before_request` hook, uses `secrets.compare_digest()` for constant-time comparison
- `web/tests/test_auth.py` — tests: no auth by default, API key required when set, basic auth required when set, wrong credentials rejected

**Files to modify**:
- `web/app.py` — call `auth.init_auth(app)` in `create_app()`, replace default `SECRET_KEY` with `secrets.token_hex(32)`

**Verification**: `pytest web/tests/test_auth.py`

---

## Dependency Graph

```
1.1 (Python ctest) ─── independent, do first
1.2 (ARM64 tests) ──── independent
1.3 (fixtures) ──────── independent, most complex C++
2.1 (benchmarks) ────── partially depends on 1.3 (minidump bench uses fixtures)
3.1 (grouping) ──────── independent of C++ work
3.2 (auth) ──────────── should come after 3.1
```

## Implementation Order

1. **1.1** Python ctest wiring (smallest change, immediate CI value)
2. **1.2** ARM64 register tests (small refactor + tests)
3. **1.3** Synthetic fixtures (most complex, high CI value)
4. **2.1** Benchmarks (depends partially on 1.3)
5. **3.1** Crash grouping/trends
6. **3.2** Authentication

## Verification (end-to-end)

After all changes:
```bash
cmake -B build && cmake --build build
ctest --test-dir build                    # all C++ + Python tests, 0 skips
ctest --test-dir build -L python          # Python tests only
cmake -B build -DENABLE_BENCHMARKS=ON && cmake --build build
./build/bench/crashomon_bench             # benchmarks run
pytest web/tests/                         # all Python tests pass
```
