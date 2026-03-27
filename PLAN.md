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
| `lib/` → `libcrashomon.so` | C11 | LD_PRELOAD / linkable client library |
| `daemon/` → `crashomon-watcherd` | C++17 | inotify watcher, tombstone formatter, disk manager |
| `tools/analyze/` → `crashomon-analyze` | C++17 | CLI: symbolicate minidump or tombstone text |
| `tools/syms/crashomon-syms` | Python | Ingest debug binaries into symbol store |
| `web/` → `crashomon-web` | Python/Flask | Web UI: symbol store + crash history + analysis |

## Dependencies (reused)

| Dependency | What we use |
|------------|------------|
| sentry-native (Crashpad backend) | Crash capture, out-of-process minidump writing |
| crashpad_handler | ptrace-based minidump writer (via sentry-native) |
| Breakpad minidump-processor | Minidump parsing in watcherd |
| Breakpad minidump_stackwalk | Offline symbolication in analyze/web |
| Breakpad dump_syms | DWARF → .sym extraction (build-time) |

## Build commands

```bash
# Build all (C/C++ components)
cmake -B build && cmake --build build

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
- [ ] Top-level `CMakeLists.txt` with compiler flags, FetchContent stubs
- [ ] `.clang-format` (Google style)
- [ ] `.clang-tidy` config
- [ ] `pyproject.toml` (ruff)
- [ ] GoogleTest scaffolding (`test/CMakeLists.txt`)
- [ ] pytest scaffolding (`web/tests/conftest.py`)
- [ ] Verify: empty test suite builds and passes

### Phase 1 — Client library
- [ ] `lib/crashomon.h` + `lib/crashomon.c`
- [ ] LD_PRELOAD constructor/destructor
- [ ] sentry_init with Crashpad backend, env var config
- [ ] Public API: set_tag, add_breadcrumb, set_abort_message

### Phase 2 — Watcher daemon
- [ ] `daemon/tombstone_formatter.cpp/.h` + unit tests
- [ ] `daemon/disk_manager.cpp/.h` + unit tests
- [ ] `daemon/minidump_reader.cpp/.h` + unit tests (using fixture .dmp files)
- [ ] `daemon/main.cpp` (inotify loop)
- [ ] systemd unit file

### Phase 3 — Analysis tools
- [ ] `tools/analyze/log_parser.cpp/.h` + unit tests
- [ ] `tools/analyze/minidump_analyzer.cpp/.h`
- [ ] `tools/analyze/symbolizer.cpp/.h`
- [ ] `tools/analyze/main.cpp`
- [ ] `tools/syms/crashomon-syms` script

### Phase 4 — Web UI
- [ ] `web/symbol_store.py` + pytest tests
- [ ] `web/models.py` + pytest tests
- [ ] `web/analyzer.py`
- [ ] `web/app.py` Flask routes + pytest tests
- [ ] Templates

### Phase 5 — Examples & integration tests
- [ ] `examples/segfault.c`, `abort.c`, `multithread_crash.c`
- [ ] Generate test fixtures (on Linux)
- [ ] Integration test scripts

## Open questions

- Crashpad database directory structure: exact path where minidumps land
- `crashpad_handler` lifecycle with multiple LD_PRELOAD processes (shared handler?)
- Breakpad minidump-processor CMake build
- Cross-compilation story for ARM/ARM64 target
