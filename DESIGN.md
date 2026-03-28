# Crashomon — Crash Monitoring Ecosystem for Embedded Linux

## Context

We're building a crash monitoring ecosystem for ELF binaries on an embedded Linux system (4GB RAM, 2GB disk, 4 cores, systemd). The system runs multiple independent processes and needs seamless crash reporting — similar to Android's tombstone/debuggerd output.

After evaluating multiple approaches, we chose **sentry-native with Crashpad backend** for crash capture + an **inotify-based watcher service** for post-crash tombstone formatting.

---

## Approach Comparison

### Why Crashpad + inotify watcher over inproc

| Aspect | sentry-native inproc | sentry-native Crashpad + inotify watcher |
|---|---|---|
| **Crash capture** | In-process signal handler | Out-of-process `crashpad_handler` via ptrace |
| **Crash data quality** | JSON envelope; uncertain multi-thread support | Real Breakpad minidump; all threads, all registers, 128KB stack per thread |
| **Tombstone formatting** | In signal handler — must be async-signal-safe (no malloc, no printf, no snprintf). Painful to implement correctly. | In a normal watcher process — unrestricted, use any library. Easy. |
| **Tombstone timing** | Immediate (at crash time) | Near-realtime (inotify fires within ms of minidump write) |
| **Offline analysis format** | JSON envelope + `eu-addr2line` (custom pipeline) | Minidump + `minidump_stackwalk` (standard, battle-tested) |
| **Multi-thread stacks** | Unclear — inproc may only capture crashing thread | Yes — Crashpad captures all threads via ptrace |
| **Abort message capture** | Via `on_crash` callback (but event is sparse) | Via Crashpad annotations (set in client, embedded in minidump) |
| **Process overhead** | None | `crashpad_handler` (~2-5MB idle) + watcher service (~2-5MB idle). Total ~4-10MB, well under 20MB budget. |
| **Language** | Pure C | C++ (Crashpad) + C (client shim) + C++ (watcher) |
| **LD_PRELOAD** | Works natively via `sentry_init()` in constructor | Works — constructor calls `sentry_init()` which spawns/connects to `crashpad_handler` |
| **Build complexity** | Simple (inproc is pure C, no extra deps) | Medium (Crashpad needs C++17, zlib; but sentry-native's CMake handles it) |
| **Reliability** | In-process — if process memory is corrupted, handler may fail | Out-of-process — handler runs in healthy process, more reliable |
| **CLI/Web UI** | Must parse custom JSON envelope format | Uses `minidump_stackwalk` — standard tool, well-documented format |

### Why sentry-native over raw Crashpad

| Aspect | Raw Crashpad | sentry-native (Crashpad backend) |
|---|---|---|
| **Build system** | GN (Chromium) or backtrace-labs CMake fork | CMake via sentry-native (vendors Crashpad as submodule) |
| **Init API** | `CrashpadClient::StartHandler()` with many params | `sentry_init(options)` — simpler C API |
| **Annotations** | `SimpleStringDictionary`, 256B per key/value, 64 entries max | `sentry_set_tag()`, `sentry_add_breadcrumb()` — richer API, auto-serialized |
| **Database management** | Manual via `CrashReportDatabase` API | Handled automatically; envelopes stored in database path |
| **Maintenance** | Must track Crashpad upstream or backtrace-labs fork | sentry-native team maintains Crashpad integration |
| **Upload (if ever needed)** | Must implement HTTP transport | Built-in (just set a DSN later) |

### Why not fully custom (build everything ourselves)

| Aspect | Fully custom | sentry-native + watcher |
|---|---|---|
| **Crash capture code** | ~1500 LOC C++, must handle ptrace edge cases, architecture-specific registers, corrupted process state | Battle-tested (Crashpad, used by Chrome/Electron) |
| **Minidump writer** | ~1000 LOC C++, fiddly format with many structures | Crashpad's writer, correct and complete |
| **Signal handler** | Must implement async-signal-safe IPC | sentry-native handles it |
| **Time to working prototype** | Weeks | Days |
| **Bug surface area** | Large — ptrace, signals, minidump format all error-prone | Small — we only write the watcher + formatting |
| **Maintenance burden** | Must track kernel/ABI changes, new architectures | Community-maintained |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      ON TARGET DEVICE                       │
│                                                             │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  Process (loaded via LD_PRELOAD or explicit link)     │  │
│  │                                                       │  │
│  │  libcrashomon.so                                      │  │
│  │  ├─ constructor: sentry_init(crashpad backend)        │  │
│  │  ├─ optional: sentry_set_tag(), add_breadcrumb()      │  │
│  │  └─ destructor: sentry_close()                        │  │
│  └──────────────┬────────────────────────────────────────┘  │
│                 │ Unix socket (on crash)                     │
│                 ▼                                            │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  crashpad_handler (spawned by sentry_init)            │  │
│  │  ├─ ptrace: reads registers, stacks, /proc/pid/maps  │  │
│  │  └─ writes minidump to /var/crashomon/                │  │
│  └──────────────┬────────────────────────────────────────┘  │
│                 │ inotify (new file event)                   │
│                 ▼                                            │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  crashomon-watcherd (systemd service)                 │  │
│  │  ├─ watches /var/crashomon/ for new minidumps         │  │
│  │  ├─ parses minidump → extracts crash info             │  │
│  │  ├─ prints Android-style tombstone to journald        │  │
│  │  └─ manages disk (prunes old minidumps)               │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                   ON DEVELOPER MACHINE                       │
│                                                             │
│  ┌──────────────────┐    ┌────────────────────────────────┐ │
│  │ crashomon-web    │───▶│ crashomon-analyze              │ │
│  │ (Flask)          │    │ ├─ reads minidump              │ │
│  │ Upload .dmp or   │    │ ├─ calls minidump_stackwalk    │ │
│  │ paste crash log  │    │ │   with debug symbols (.sym)  │ │
│  └──────────────────┘    │ └─ outputs symbolicated report │ │
│                          └────────────────────────────────┘ │
│                                                             │
│  ┌────────────────────────────────────────────────────────┐ │
│  │ dump_syms (Breakpad, build-time)                       │ │
│  │ Extracts DWARF debug info → .sym files                 │ │
│  └────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## Components

### 1. `libcrashomon.so` — Client Library (LD_PRELOAD / linkable)

**Purpose**: Automatically instruments any dynamically-linked process for crash reporting.

**Implementation**:
- Thin C shared library wrapping sentry-native (statically linked in, Crashpad backend)
- `__attribute__((constructor))` calls `sentry_init()` with:
  - `SENTRY_BACKEND=crashpad`
  - `SENTRY_TRANSPORT=none` (no uploads)
  - Handler path: `/usr/libexec/crashomon/crashpad_handler` (configurable via `CRASHOMON_HANDLER_PATH` env var)
  - Database path: `/var/crashomon/` (configurable via `CRASHOMON_DB_PATH` env var)
  - DSN: dummy value (no server)
- `__attribute__((destructor))` calls `sentry_close()`
- Optional public API for explicit users (not LD_PRELOAD):
  - `crashomon_init(config)` — manual init with custom config
  - `crashomon_set_tag(key, value)` — wraps `sentry_set_tag()`
  - `crashomon_add_breadcrumb(message)` — wraps `sentry_add_breadcrumb()`
  - `crashomon_set_abort_message(msg)` — sets annotation for abort message capture

**Files**:
- `lib/crashomon.h` — public API header (pure C, C-compatible, freestanding — no project deps required to consume)
- `lib/crashomon.cpp` — C++17 implementation; constructor/destructor, init logic, API wrappers
- `lib/CMakeLists.txt`

**Build output**: `libcrashomon.so` (shared, for LD_PRELOAD) + `libcrashomon.a` (static, for explicit linking) + `crashpad_handler` binary

---

### 2. `crashomon-watcherd` — Watcher Daemon (systemd service)

**Purpose**: Watches for new minidumps, formats and logs Android-style crash tombstones, manages disk.

**Implementation** (C++):
- Uses `inotify` to watch the Crashpad database directory for new minidump files
- On new minidump:
  1. Waits briefly for write completion (file size stabilization or Crashpad database lock release)
  2. Parses minidump using Breakpad's `minidump-processor` library (or a lightweight minidump reader)
  3. Extracts: PID, TID, process name, signal info, registers (all threads), stack traces (raw addresses + module offsets), loaded modules
  4. Formats Android-style tombstone
  5. Writes to journald via `sd_journal_send()` (or stderr for non-systemd fallback)
- Disk management:
  - Configurable max database size (e.g., 100MB)
  - Prunes oldest minidumps when limit is exceeded
  - Configurable max age (e.g., 7 days)

**Crash output format** (Android-inspired):
```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 1234, tid: 1234, name: my_service  >>> my_service <<<
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0000dead
timestamp: 2026-03-27T10:15:30Z

    rax 0000000000000000  rbx 00007fff5fbff8a0  rcx 0000000000000001
    rdx 0000000000000000  rsi 00007fff5fbff8a0  rdi 0000000000000001
    rbp 00007fff5fbff870  rsp 00007fff5fbff850  rip 00005555555551a0
    ... (all registers for crashing thread)

backtrace:
    #00 pc 0x00001a0  /usr/bin/my_service
    #01 pc 0x0000f32  /usr/bin/my_service
    #02 pc 0x0023b09  /usr/lib/libc.so.6
    #03 pc 0x0023a90  /usr/lib/libc.so.6

--- --- --- thread 1235 --- --- ---
    #00 pc 0x000ab12  /usr/lib/libc.so.6
    #01 pc 0x0000c44  /usr/bin/my_service

minidump saved to: /var/crashomon/abcdef12-3456-7890-abcd-ef1234567890.dmp
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Dependencies**: Breakpad's minidump processor library (for parsing minidumps), libsystemd (for journald integration)

**Files**:
- `daemon/main.cpp` — entry point, inotify loop
- `daemon/minidump_reader.cpp/.h` — wraps Breakpad minidump processor for reading minidumps
- `daemon/tombstone_formatter.cpp/.h` — formats crash info into Android-style text
- `daemon/disk_manager.cpp/.h` — prunes old minidumps based on size/age
- `daemon/CMakeLists.txt`

**systemd unit**:
```ini
[Unit]
Description=Crashomon Crash Watcher
After=local-fs.target

[Service]
ExecStart=/usr/libexec/crashomon/crashomon-watcherd --db-path=/var/crashomon --max-size=100M --max-age=7d
Restart=always
MemoryMax=20M

[Install]
WantedBy=multi-user.target
```

---

### 3. Symbol Store

**Purpose**: A build-ID-indexed repository of Breakpad `.sym` files, enabling automatic symbol lookup when analyzing any minidump from any version of any process.

**How it works**: Breakpad's standard symbol store layout is:
```
<symbol_root>/
├── my_service/
│   ├── ABCDEF1234567890ABCDEF1234567890A/    ← build ID (from ELF .note.gnu.build-id)
│   │   └── my_service.sym                     ← Breakpad symbol file
│   └── 1234567890ABCDEF1234567890ABCDEF1/    ← older version
│       └── my_service.sym
├── libc.so.6/
│   └── 9876543210FEDCBA9876543210FEDCBA0/
│       └── libc.so.6.sym
└── libpthread.so.0/
    └── ...
```

`minidump_stackwalk <minidump> <symbol_root>` automatically matches modules by name + build ID. A minidump from v1.2 of `my_service` finds the v1.2 symbols, and a minidump from v1.3 finds v1.3 symbols — no manual matching needed.

**Symbol ingestion tool** — `crashomon-syms`:
```bash
# Ingest symbols for a single binary (runs dump_syms, stores in correct location)
crashomon-syms add --store /path/to/symbol_store /path/to/my_service

# Ingest all ELF binaries from a sysroot/build output directory
crashomon-syms add --store /path/to/symbol_store --recursive /path/to/build/output/

# List stored symbols
crashomon-syms list --store /path/to/symbol_store
# Output:
#   my_service  ABCDEF1234567890...  2026-03-25  1.2MB
#   my_service  1234567890ABCDEF...  2026-03-20  1.1MB
#   libc.so.6   9876543210FEDCBA...  2026-03-15  4.5MB

# Prune old versions (keep latest N per module)
crashomon-syms prune --store /path/to/symbol_store --keep 3
```

**Implementation**: Shell script or Python wrapper around Breakpad's `dump_syms`. The `add` command:
1. Runs `dump_syms <binary>` to produce the `.sym` file
2. Parses the first line of `.sym` output: `MODULE Linux x86_64 ABCDEF1234567890... my_service`
3. Creates `<store>/<module_name>/<build_id>/` directory
4. Moves the `.sym` file into place

**CI/CD integration**: After each build, run:
```bash
crashomon-syms add --store /shared/symbol_store --recursive /build/output/
```

**Files**:
- `tools/syms/crashomon-syms` — shell script or Python script

---

### 4. `crashomon-analyze` — CLI Analysis Tool

**Purpose**: Symbolicates a single minidump or crash log against a symbol store or explicit symbol path. Scriptable, for one-off analysis or CI integration.

**Implementation** (C++):
- For minidump input: calls `minidump_stackwalk` with the symbol store path. Build IDs in the minidump auto-resolve to the correct `.sym` files.
- For pasted crash log (tombstone text): extracts addresses + module info via regex, uses `eu-addr2line` with `.debug` files or `minidump_stackwalk`-style lookup.

**Usage**:
```bash
# Symbolicate a minidump using the symbol store (auto-matches by build ID)
crashomon-analyze --store /path/to/symbol_store --minidump crash.dmp

# Symbolicate from pasted crash log text
crashomon-analyze --store /path/to/symbol_store --stdin < tombstone.txt

# Explicit symbol path (no store, manual)
crashomon-analyze --symbols /path/to/specific.sym --minidump crash.dmp
```

**Output** (symbolicated):
```
pid: 1234, tid: 1234, name: my_service  >>> my_service <<<
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0000dead

backtrace:
    #00 pc 0x00001a0  /usr/bin/my_service (handle_request+0x20) [src/server.c:142]
    #01 pc 0x0000f32  /usr/bin/my_service (main+0x52) [src/main.c:38]
    #02 pc 0x0023b09  /usr/lib/libc.so.6 (__libc_start_call_main+0x79)
    #03 pc 0x0023a90  /usr/lib/libc.so.6 (__libc_start_main+0x90)

--- --- --- thread 1235 --- --- ---
    #00 pc 0x000ab12  /usr/lib/libc.so.6 (epoll_wait+0x22)
    #01 pc 0x0000c44  /usr/bin/my_service (worker_loop+0x14) [src/worker.c:67]
```

**Dependencies**: Breakpad's `minidump_stackwalk` + `dump_syms`, elfutils for crash-log-text mode

**Files**:
- `tools/analyze/main.cpp`
- `tools/analyze/minidump_analyzer.cpp/.h` — wraps minidump_stackwalk invocation
- `tools/analyze/log_parser.cpp/.h` — parses raw crash log text (regex on tombstone format)
- `tools/analyze/symbolizer.cpp/.h` — wraps eu-addr2line for text mode
- `tools/analyze/CMakeLists.txt`

---

### 5. `crashomon-web` — Web UI with Symbol Store Management

**Purpose**: Browser-based crash analysis hub. Manages a symbol store across multiple processes and versions. Auto-symbolicates minidumps by build ID lookup. Stores crash history.

**Key difference from CLI**: The web UI **owns and manages** the symbol store. The CLI is a stateless tool that points at a store; the web UI provides upload, browsing, versioning, and history.

**Features**:
- **Symbol management**:
  - Upload debug binaries via web form → server runs `dump_syms`, stores `.sym` in the symbol store
  - Browse stored symbols: list modules, versions (build IDs), upload dates, sizes
  - Prune old versions
  - REST API for CI/CD: `POST /api/symbols/upload` with multipart binary upload
- **Crash analysis**:
  - Upload `.dmp` minidump file → auto-symbolicated using symbol store (matched by build ID)
  - Paste tombstone text → parsed and symbolicated
  - If symbols are missing for a module/build ID, warn the user with the missing build ID
  - Show all threads, registers, signal info in a formatted view
- **Crash history** (SQLite):
  - Stores past crash reports with metadata: timestamp, process name, signal, symbolicated backtrace
  - Browse / search / filter by process name, signal type, date range
  - View individual crash details
  - Track crash frequency per process

**Implementation** (Python + Flask + SQLite):
- Flask app with REST API + Jinja2 templates
- SQLite database for crash history metadata
- Symbol store on filesystem (Breakpad layout)
- Calls `crashomon-analyze` or `minidump_stackwalk` as subprocess for symbolication

**Pages**:
| Route | Purpose |
|---|---|
| `GET /` | Dashboard: recent crashes, crash frequency per process |
| `GET /crashes` | Crash history list, filterable |
| `GET /crashes/<id>` | Individual crash detail (symbolicated report) |
| `POST /crashes/upload` | Upload minidump or paste text for analysis |
| `GET /symbols` | Browse symbol store: modules, versions, sizes |
| `POST /symbols/upload` | Upload debug binary → extract + store symbols |
| `DELETE /symbols/<module>/<build_id>` | Remove specific symbol version |
| `POST /api/symbols/upload` | REST API for CI/CD symbol upload |
| `POST /api/crashes/upload` | REST API for programmatic minidump upload |

**Files**:
- `web/app.py` — Flask application, routes, API
- `web/models.py` — SQLite schema + queries for crash history
- `web/symbol_store.py` — symbol store management (add, list, prune, lookup)
- `web/analyzer.py` — wraps crashomon-analyze / minidump_stackwalk subprocess calls
- `web/templates/base.html` — layout
- `web/templates/dashboard.html` — recent crashes, frequency chart
- `web/templates/crashes.html` — crash list
- `web/templates/crash_detail.html` — single crash report view
- `web/templates/symbols.html` — symbol store browser
- `web/templates/upload.html` — upload minidump / paste text / upload symbols
- `web/requirements.txt` — Flask, sqlite3 (stdlib)

---

### 6. Build System & Integration

**Directory structure**:
```
crashomon/
├── CMakeLists.txt              # Top-level: fetches sentry-native, builds all C/C++
├── lib/                        # libcrashomon.so/.a (client library)
│   ├── CMakeLists.txt
│   ├── crashomon.h
│   └── crashomon.cpp
├── daemon/                     # crashomon-watcherd (inotify watcher)
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── minidump_reader.cpp/.h
│   ├── tombstone_formatter.cpp/.h
│   └── disk_manager.cpp/.h
├── tools/
│   ├── analyze/                # crashomon-analyze CLI
│   │   ├── CMakeLists.txt
│   │   └── *.cpp/.h
│   └── syms/                   # crashomon-syms (symbol ingestion)
│       └── crashomon-syms      # shell/python script
├── web/                        # crashomon-web (Flask + symbol store + crash history)
│   ├── app.py
│   ├── models.py
│   ├── symbol_store.py
│   ├── analyzer.py
│   ├── templates/
│   ├── tests/                  # pytest for web components
│   │   ├── conftest.py
│   │   ├── test_symbol_store.py
│   │   ├── test_models.py
│   │   └── test_routes.py
│   └── requirements.txt
├── test/                       # C/C++ unit tests (GoogleTest)
│   ├── CMakeLists.txt
│   ├── test_tombstone_formatter.cpp
│   ├── test_disk_manager.cpp
│   ├── test_minidump_reader.cpp
│   ├── test_log_parser.cpp
│   ├── fixtures/               # Pre-generated test data (committed to repo)
│   │   ├── segfault.dmp
│   │   ├── abort.dmp
│   │   ├── multithread.dmp
│   │   ├── symbols/            # Matching .sym files
│   │   └── tombstone.txt       # Sample tombstone text
│   └── integration/            # End-to-end tests (Linux only, run in CI)
│       ├── test_crash_capture.sh
│       ├── test_watcher_e2e.sh
│       └── test_cross_version_syms.sh
├── examples/                   # Example crasher programs for testing
│   ├── CMakeLists.txt
│   ├── segfault.c
│   ├── abort.c
│   └── multithread_crash.c
└── systemd/                    # systemd unit files
    ├── crashomon-watcherd.service
    └── example-app.service.d/
        └── crashomon.conf      # Drop-in showing LD_PRELOAD usage
```

**sentry-native integration** (CMake FetchContent):
```cmake
FetchContent_Declare(sentry
  GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
  GIT_TAG <pinned-release-tag>
)
set(SENTRY_BACKEND "crashpad" CACHE STRING "" FORCE)
set(SENTRY_TRANSPORT "none" CACHE STRING "" FORCE)
set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(SENTRY_PIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sentry)
```

**Breakpad** (for minidump-processor library, `minidump_stackwalk`, and `dump_syms`):
FetchContent from upstream Breakpad. Custom CMake wrapper at `cmake/breakpad.cmake` defines targets
since Breakpad has no native CMake build. Targets: `breakpad_processor` (library), `minidump_stackwalk`
(executable), `breakpad_dump_syms` (executable), `breakpad_libdisasm` (internal, vendored disassembler).
All Breakpad targets compiled with `-w` (no warnings-as-errors for third-party code).

**Abseil** (internal error handling):
FetchContent. Granular CMake targets — only `absl::status` and `absl::statusor` linked per component.
Internal only: `absl::Status`/`absl::StatusOr` must not appear in public interfaces.

**Google Benchmark** (opt-in microbenchmarks):
FetchContent, gated behind `-DENABLE_BENCHMARKS=ON`.

---

## What we build vs. reuse

| Component | Source | Effort |
|---|---|---|
| Crash capture + signal handling | **Reuse**: sentry-native (Crashpad backend) | 0 |
| Out-of-process minidump writing | **Reuse**: crashpad_handler (via sentry-native) | 0 |
| LD_PRELOAD shim + sentry init | **Build**: ~150-250 LOC C++17 (C API) | Low |
| Watcher daemon (inotify + formatting) | **Build**: ~800-1200 LOC C++ | Medium |
| Minidump parsing (in watcher) | **Reuse**: Breakpad minidump processor | 0 |
| Symbol ingestion tool | **Build**: ~100-200 LOC script (wraps dump_syms) | Low |
| Symbol store layout | **Reuse**: Breakpad standard layout (module/build_id/module.sym) | 0 |
| CLI analysis tool | **Build**: ~400-600 LOC C++ (wraps minidump_stackwalk) | Low |
| Symbolication engine | **Reuse**: Breakpad minidump_stackwalk + dump_syms | 0 |
| Web UI + symbol store mgmt + crash history | **Build**: ~600-900 LOC Python | Medium |
| systemd integration | **Build**: unit files | Low |

**Total custom code: ~2000-3000 LOC** (C + C++ + Python + script)

---

## Verification / Test Plan

1. **Build test**: `cmake -B build && cmake --build build` succeeds on Linux
2. **Crash capture test**: Run `examples/segfault` with `LD_PRELOAD=libcrashomon.so`. Verify minidump appears in `/var/crashomon/`
3. **Watcher test**: Start `crashomon-watcherd`, trigger a crash, verify Android-style tombstone appears in journald (`journalctl -u crashomon-watcherd`)
4. **Multi-thread test**: Run `examples/multithread_crash` with LD_PRELOAD. Verify tombstone shows all threads' stacks
5. **LD_PRELOAD inheritance test**: Bash script spawning 3 processes, crash one, verify only that one is reported
6. **Symbol ingestion test**: Run `crashomon-syms add --store /tmp/symbols /path/to/my_service`. Verify `.sym` file stored at `<store>/<module>/<build_id>/<module>.sym`
7. **CLI symbolication test**: `crashomon-analyze --store <symbol-store> --minidump <file>` outputs function names + line numbers
8. **Cross-version symbolication test**: Build two versions of `examples/segfault` with different code, ingest both into the symbol store, crash both, verify each minidump symbolicates against the correct version's symbols
9. **Crash log paste test**: `echo "<tombstone text>" | crashomon-analyze --store <store> --stdin` symbolicates from text
10. **Web UI — symbol upload**: Upload a debug binary via web form, verify it appears in the symbol browser with correct module name + build ID
11. **Web UI — CI/CD API**: `curl -F binary=@my_service POST http://localhost:5000/api/symbols/upload`, verify symbols stored
12. **Web UI — minidump analysis**: Upload a `.dmp` via web form, verify auto-symbolicated report with correct function names
13. **Web UI — missing symbols**: Upload a minidump for a version whose symbols are NOT in the store, verify clear error showing the missing build ID
14. **Web UI — crash history**: Upload multiple minidumps, verify crash list shows all with correct metadata, filter by process name works
15. **Disk management test**: Generate many crashes, verify watcher prunes old minidumps at configured limit

---

## Code Quality & Build Guardrails

### Compiler flags (CMake)

```cmake
# Language standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Applied to ALL targets (including deps — consistent ABI, no exception mismatch)
add_compile_options(-O3 -g)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

# Applied ONLY to our targets via crashomon_warnings interface library
# (not to third-party deps — avoids -Werror breaking sentry-native, Breakpad, etc.)
add_library(crashomon_warnings INTERFACE)
target_compile_options(crashomon_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wformat-security
  -fstack-protector-strong
)
```

Third-party deps (sentry-native, GoogleTest, Abseil, Breakpad) are compiled with `-w`
(suppress all warnings) where needed. They must NOT inherit `-Werror`.

### clang-format

`.clang-format` at repo root. Style based on LLVM with project-specific tweaks:

```yaml
BasedOnStyle: Google
ColumnLimit: 100
SortIncludes: CaseSensitive
IncludeBlocks: Regroup
```

**Enforcement**:
- Pre-commit check: `clang-format --dry-run --Werror` on changed files
- CI: `clang-format --dry-run --Werror $(find lib daemon tools -name '*.c' -o -name '*.cpp' -o -name '*.h')`

### C++ style guidelines

Follow [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) and [Abseil's Tips of the Week](https://abseil.io/tips/). Key practices:
- Prefer `absl::string_view` or `std::string_view` over `const std::string&` for read-only string parameters ([TotW #1](https://abseil.io/tips/1))
- Use `auto` only when the type is obvious from context ([TotW #42](https://abseil.io/tips/42))
- Prefer `enum class` over plain `enum` ([TotW #86](https://abseil.io/tips/86))
- Return by value, rely on copy elision ([TotW #77](https://abseil.io/tips/77))
- Prefer `std::make_unique` over `new` ([TotW #126](https://abseil.io/tips/126))
- Use structured bindings where they improve readability ([TotW #140](https://abseil.io/tips/140))
- Avoid `std::move` on return statements (defeats NRVO) ([TotW #11](https://abseil.io/tips/11))
- **No exceptions**: use `absl::Status` and `absl::StatusOr<T>` for error handling. All C++ is compiled with `-fno-exceptions`. Never use `try`/`catch`/`throw`.
- **Freestanding public interfaces**: `absl::Status`/`absl::StatusOr` and other Abseil types must not appear in public or cross-library headers. Consumers of `lib/crashomon.h` must not need to add Abseil as a dependency.

### clang-tidy

`.clang-tidy` at repo root. Enabled check groups:

```yaml
Checks: >
  -*,
  bugprone-*,
  cert-*,
  clang-analyzer-*,
  cppcoreguidelines-*,
  misc-*,
  modernize-*,
  performance-*,
  readability-*,
  -modernize-use-trailing-return-type,
  -readability-magic-numbers,
  -cppcoreguidelines-avoid-magic-numbers
WarningsAsErrors: '*'
HeaderFilterRegex: '(lib|daemon|tools)/.*'
```

**Enforcement**:
- CMake integration: `set(CMAKE_CXX_CLANG_TIDY "clang-tidy")` when `-DENABLE_CLANG_TIDY=ON`
- CI: `cmake -B build -DENABLE_CLANG_TIDY=ON && cmake --build build`

### Python linting

- `ruff` for linting + formatting (replaces flake8 + black)
- `pyproject.toml` config:
  ```toml
  [tool.ruff]
  line-length = 100
  target-version = "py311"

  [tool.ruff.lint]
  select = ["E", "F", "W", "I", "N", "UP", "B", "SIM", "S"]
  ```
- CI: `ruff check web/ && ruff format --check web/`

### Sanitizers

Sanitizers are opt-in via CMake options, no separate build types. The default build uses `-O3 -g` (optimized with debug info).

```cmake
# Default: optimized build with debug info (no separate Debug/Release)
add_compile_options(-O3 -g)

# Sanitizer options (toggle manually)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)

if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()

if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
```

**Usage**:
```bash
# Normal build
cmake -B build && cmake --build build

# Run tests with UBSan
cmake -B build-ubsan -DENABLE_UBSAN=ON && cmake --build build-ubsan && ctest --test-dir build-ubsan

# Run tests with TSan
cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan && ctest --test-dir build-tsan
```

Note: ASan and TSan cannot be used simultaneously (they conflict). UBSan can be combined with either.

### Test design for sanitizer coverage

Tests must exercise code paths that surface UBSan and TSan issues:

**UBSan-targeted tests**:
- `test_tombstone_formatter`: integer overflow in offset calculations, signed/unsigned conversions, null pointer in optional fields, format string with edge-case values (0, UINT64_MAX, negative fault addresses)
- `test_minidump_reader`: malformed/truncated minidump files, zero-length stack memory, missing thread entries, corrupt module list
- `test_log_parser`: malformed tombstone text, empty input, extremely long lines, non-UTF8 bytes
- `test_disk_manager`: empty directory, single file at boundary size, zero-size limit

**TSan-targeted tests** (for components with concurrency):
- `test_watcher_concurrent`: simulate multiple minidumps arriving simultaneously (watcher may process in parallel or queue)
- `test_disk_manager_concurrent`: concurrent prune + new file arrival
- If any component uses shared state or threads, tests must exercise concurrent access patterns

### CI pipeline

Single build configuration with optional sanitizer passes:

```bash
# 1. Build + test (default)
cmake -B build && cmake --build build && ctest --test-dir build

# 2. clang-tidy
cmake -B build-tidy -DENABLE_CLANG_TIDY=ON && cmake --build build-tidy

# 3. clang-format check
clang-format --dry-run --Werror $(find lib daemon tools -name '*.c' -o -name '*.cpp' -o -name '*.h')

# 4. UBSan
cmake -B build-ubsan -DENABLE_UBSAN=ON && cmake --build build-ubsan && ctest --test-dir build-ubsan

# 5. TSan
cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan && ctest --test-dir build-tsan

# 6. Python lint + test
ruff check web/ && ruff format --check web/ && cd web && pytest tests/
```

### Implementation order for guardrails (Phase 0)

Before writing any component code:
1. Create top-level `CMakeLists.txt` with compiler flags, FetchContent for sentry-native and GoogleTest
2. Create `.clang-format` and `.clang-tidy` configs
3. Create `pyproject.toml` with ruff config
4. Create `test/CMakeLists.txt` with GoogleTest scaffolding
5. Create `web/tests/conftest.py` with pytest fixtures
6. Verify: `cmake -B build && cmake --build build && ctest --test-dir build` passes (empty test suite)
7. Verify: `clang-format` and `clang-tidy` run without errors on empty/stub files

---

## Unit Testing

**Frameworks**: GoogleTest for C/C++ (fetched via CMake FetchContent), pytest for Python.

### C/C++ unit tests (GoogleTest)

All C++ modules are designed with testable interfaces — pure functions taking data structs, no platform coupling in core logic.

| Test file | Module under test | What it tests |
|---|---|---|
| `test_tombstone_formatter.cpp` | `daemon/tombstone_formatter` | Feed `CrashInfo` structs → assert formatted tombstone string matches expected output. Tests: single thread, multi-thread, all signal types, register formatting, module offset calculation. |
| `test_disk_manager.cpp` | `daemon/disk_manager` | Create temp dir with fake `.dmp` files of varying ages/sizes → call prune → assert correct files remain. Tests: max size limit, max age limit, keep-latest behavior. |
| `test_minidump_reader.cpp` | `daemon/minidump_reader` | Read fixture `.dmp` files → assert extracted CrashInfo fields match expected values (PID, signal, registers, thread count, module list). |
| `test_log_parser.cpp` | `tools/analyze/log_parser` | Feed tombstone text strings → assert parsed addresses, module names, signal info, thread structure. Tests: complete tombstone, partial/truncated, malformed input. |

**Fixture generation**: Fixture `.dmp` and `.sym` files are generated once on Linux (using the example crashers + `dump_syms`) and committed to the repo. A helper script `test/generate_fixtures.sh` regenerates them when needed.

**CMake integration**:
```cmake
enable_testing()
FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.14.0)
FetchContent_MakeAvailable(googletest)
add_subdirectory(test)
# Run with: cmake --build build --target test
```

### Python tests (pytest)

| Test file | Module under test | What it tests |
|---|---|---|
| `test_symbol_store.py` | `web/symbol_store.py` | Add `.sym` to temp store → verify file at `<module>/<build_id>/<module>.sym`. List → verify output. Prune with `keep=2` → verify oldest removed. Lookup by build ID → verify correct path or missing. |
| `test_models.py` | `web/models.py` | CRUD on in-memory SQLite: insert crash record, query by process name, query by date range, verify crash frequency aggregation. |
| `test_routes.py` | `web/app.py` | Flask test client: POST minidump upload → assert 200 + symbolicated output (mock analyzer subprocess). POST symbol upload → assert stored. GET crash list → assert JSON/HTML. GET with missing symbols → assert warning. |

**Run with**: `cd web && pytest tests/`

### Integration tests (Linux only, CI)

Shell scripts that test the full end-to-end flow:

| Script | What it tests |
|---|---|
| `test_crash_capture.sh` | Build example crasher → `LD_PRELOAD=libcrashomon.so ./segfault` → assert minidump exists in database path |
| `test_watcher_e2e.sh` | Start watcherd → trigger crash → capture watcherd stdout → assert tombstone format correct |
| `test_cross_version_syms.sh` | Build two versions of crasher with different source → ingest both into symbol store → crash both → symbolicate both → assert each resolves to correct source lines |

---

## Open questions / risks

- **Crashpad database directory structure**: Need to verify the exact path where minidumps land and whether inotify on that directory is sufficient (Crashpad may use temp files + rename, which inotify `IN_MOVED_TO` handles)
- **Minidump processor library extraction**: The watcher needs to parse minidumps. We need Breakpad's `minidump-processor` library linked into the watcher. Need to verify this can be cleanly built with CMake.
- **`crashpad_handler` lifecycle with LD_PRELOAD**: When multiple processes init via LD_PRELOAD, do they each spawn a separate `crashpad_handler`, or do they share one? sentry-native may handle this (Crashpad supports a shared handler), but needs verification.
- **Cross-compilation**: Building sentry-native (with Crashpad) + Breakpad processor for embedded ARM/ARM64 target needs testing.
- **RAM budget**: `crashpad_handler` (~2-5MB) + `crashomon-watcherd` (~2-5MB) + per-process client overhead. Should stay well under 20MB but needs measurement on target.
