# Crashomon — Implementation Plan

Crash monitoring ecosystem for ELF binaries on embedded Linux (4GB RAM, 2GB disk, 4 cores, systemd).

## Architecture

```
ON TARGET DEVICE
┌─────────────────────────────────────────────────────────────┐
│  Process (via LD_PRELOAD or explicit link)                  │
│  └─ libcrashomon.so                                         │
│     ├─ constructor: sentry_init(crashpad backend)           │
│     └─ API: set_tag, add_breadcrumb, set_abort_message      │
│                          │ Unix socket (on crash)           │
│                          ▼                                  │
│  crashpad_handler (spawned by sentry_init)                  │
│  ├─ ptrace: reads registers, stacks, /proc/pid/maps         │
│  └─ writes minidump to /var/crashomon/                      │
│                          │ inotify                          │
│                          ▼                                  │
│  crashomon-watcherd (systemd service)                       │
│  ├─ watches /var/crashomon/ for new minidumps               │
│  ├─ parses minidump (Breakpad processor)                    │
│  ├─ prints Android-style tombstone to journald              │
│  └─ prunes old minidumps (configurable size/age limit)      │
└─────────────────────────────────────────────────────────────┘

ON DEVELOPER MACHINE
┌─────────────────────────────────────────────────────────────┐
│  crashomon-web (Flask)   ──▶  crashomon-analyze (CLI)       │
│  - Symbol store manager         - Wraps minidump_stackwalk  │
│  - Crash history (SQLite)       - Wraps eu-addr2line        │
│  - Build ID auto-lookup         - Stateless, scriptable     │
│  - CI/CD REST API                                           │
│                                                             │
│  Symbol store (Breakpad layout):                            │
│  <root>/<module>/<build_id>/<module>.sym                    │
│                                                             │
│  crashomon-syms: ingests debug binaries → symbol store      │
└─────────────────────────────────────────────────────────────┘
```

## Components

| Component | Language | Role |
|-----------|----------|------|
| `lib/` → `libcrashomon.so` | C++20 (C API) | LD_PRELOAD / linkable client library; C-compatible public header |
| `daemon/` → `crashomon-watcherd` | C++20 | inotify watcher, tombstone formatter, disk manager |
| `tools/analyze/` → `crashomon-analyze` | C++20 | CLI: symbolicate minidump or tombstone text |
| `tools/syms/crashomon-syms` | Python | Ingest debug binaries into symbol store |
| `web/` → `crashomon-web` | Python/Flask | Web UI: symbol store + crash history + analysis |

## Dependencies (reused)

| Dependency | What we use |
|------------|------------|
| sentry-native (Crashpad backend) | Crash capture, out-of-process minidump writing |
| crashpad_handler | ptrace-based minidump writer (via sentry-native) |
| Abseil (`absl::Status`, `absl::StatusOr`) | Internal error handling — no exceptions |
| Breakpad minidump-processor | Minidump parsing in watcherd |
| Breakpad `minidump_stackwalk` | Offline symbolication in analyze/web |
| Breakpad `dump_syms` | DWARF → .sym extraction (build-time) |
| Google Benchmark | Microbenchmarks (opt-in: `-DENABLE_BENCHMARKS=ON`) |

All C/C++ dependencies are fetched via CMake FetchContent — no system-install required.
Exception: zlib (system library, required by Breakpad: `find_package(ZLIB REQUIRED)`).

## Constraints

- **`-fno-exceptions`**: All C++ compiled without exceptions. Use `absl::Status`/`absl::StatusOr` for error handling.
- **Freestanding public APIs**: No Abseil types in public or cross-library interfaces. Public C API (`lib/crashomon.h`) uses only C types. `absl::Status`/`absl::StatusOr` confined to `.cpp` and internal headers.
- **FetchContent for all project deps**: sentry-native, GoogleTest, Abseil, Breakpad (custom CMake wrapper in `cmake/breakpad.cmake`), Google Benchmark.

## Build commands

```bash
# Build all (C/C++ components)
cmake -B build && cmake --build build

# With tests and benchmarks
cmake -B build -DENABLE_TESTS=ON -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" && cmake --build build

# Run unit tests
ctest --test-dir build

# With sanitizers (pick one)
cmake -B build-ubsan -DENABLE_UBSAN=ON && cmake --build build-ubsan && ctest --test-dir build-ubsan
cmake -B build-tsan  -DENABLE_TSAN=ON  && cmake --build build-tsan  && ctest --test-dir build-tsan

# Static analysis
cmake -B build-tidy -DENABLE_CLANG_TIDY=ON && cmake --build build-tidy

# Format check
clang-format --dry-run --Werror $(find lib daemon tools -name '*.c' -o -name '*.cpp' -o -name '*.h')

# Python
cd web && ruff check . && ruff format --check . && pytest tests/
```

## Implementation phases

### Phase 0 — Guardrails (do this first)
- [x] Top-level `CMakeLists.txt` with compiler flags, FetchContent stubs
- [x] `.clang-format` (Google style)
- [x] `.clang-tidy` config
- [x] `pyproject.toml` (ruff)
- [x] GoogleTest scaffolding (`test/CMakeLists.txt`)
- [x] pytest scaffolding (`web/tests/conftest.py`)
- [x] Verify: empty test suite builds and passes

### Phase 1 — Client library
- [x] `lib/crashomon.h` + `lib/crashomon.cpp` (C++20 implementation, C-compatible header)
- [x] LD_PRELOAD constructor/destructor
- [x] sentry_init with Crashpad backend, env var config
- [x] Public API: set_tag, add_breadcrumb, set_abort_message

### Phase 2 — Watcher daemon
- [x] `daemon/tombstone_formatter.cpp/.h` + unit tests (14 tests)
- [x] `daemon/disk_manager.cpp/.h` + unit tests (11 tests)
- [x] `daemon/minidump_reader.cpp/.h` (fixture-based tests deferred to Phase 5)
- [x] `daemon/main.cpp` (inotify loop, SIGTERM, arg parsing)
- [x] systemd unit file

### Phase 3 — Analysis tools
- [x] `tools/analyze/log_parser.cpp/.h` + unit tests (26 tests)
- [x] `tools/analyze/minidump_analyzer.cpp/.h`
- [x] `tools/analyze/symbolizer.cpp/.h`
- [x] `tools/analyze/main.cpp`
- [x] `tools/syms/crashomon-syms` script

### Phase 4 — Web UI
- [x] `web/symbol_store.py` + pytest tests (22 tests)
- [x] `web/models.py` + pytest tests (20 tests)
- [x] `web/analyzer.py`
- [x] `web/app.py` Flask routes + pytest tests (27 tests)
- [x] Templates (base, dashboard, crashes, crash_detail, symbols, upload)

### Phase 5 — Examples & integration tests
- [x] `examples/segfault.c`, `abort.c`, `multithread_crash.c`
- [x] Generate test fixtures (on Linux) — `test/gen_fixtures.sh`
- [x] Integration test scripts — `test/integration_test.sh`

## Open questions

- Crashpad database directory structure: exact path where minidumps land
- `crashpad_handler` lifecycle with multiple LD_PRELOAD processes (shared handler?)
- Cross-compilation story for ARM/ARM64 target
