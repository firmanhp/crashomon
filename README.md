# Crashomon

Crash monitoring ecosystem for ELF binaries on embedded Linux. Provides Android tombstone-style crash reports, Breakpad minidumps, and a web UI for symbolicated offline analysis — with zero code changes required via `LD_PRELOAD`.

---

## Architecture

![Architecture](docs/architecture.svg)

---

## Components

| Component | Language | Role |
|---|---|---|
| `libcrashomon.so` / `.a` | C++20 (C public API) | LD_PRELOAD crash capture client |
| `crashpad_handler` | C++ (Crashpad) | Out-of-process minidump writer |
| `crashomon-watcherd` | C++20 | inotify watcher, tombstone formatter, disk manager |
| `crashomon-analyze` | C++20 | CLI symbolication tool |
| `crashomon-syms` | Python 3.11 | Symbol store management |
| `crashomon-web` | Python/Flask | Web UI + REST API |

---

## Prerequisites

### Build machine

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.21 | |
| C++ compiler | GCC ≥ 13 or Clang ≥ 16 | Must support C++20 |
| Python | ≥ 3.11 | For `crashomon-syms` and `crashomon-web` |
| `uv` | any | Python dependency management (`pip install uv`) |
| `git` | any | FetchContent clones dependencies |
| `ninja` or `make` | any | CMake build backend |

All C/C++ dependencies (Crashpad, Breakpad, Abseil, GoogleTest, Google Benchmark, zlib) are fetched automatically by CMake — **no system-level installs required** for those.

### Target device (runtime)

| Requirement | Value |
|---|---|
| OS | Linux (systemd, kernel ≥ 4.4) |
| Architecture | x86-64 (arm64 untested) |
| RAM | ≥ 4 GB recommended |
| Disk | ≥ 2 GB (watcher + minidump store) |
| Writable path | `/var/crashomon/` (configurable) |

The watcher daemon uses ≤ 20 MB RSS at idle.

---

## Building

```bash
# Clone
git clone https://github.com/firmanhp/crashomon.git
cd crashomon

# Configure + build all C/C++ targets
cmake -B build
cmake --build build -j$(nproc)

# Install Python deps (web UI + syms script)
uv sync
```

This produces:

```
build/lib/libcrashomon.so          # LD_PRELOAD library
build/lib/libcrashomon.a           # static library
build/crashpad_build/handler/crashpad_handler  # out-of-process crash handler
build/daemon/crashomon-watcherd    # watcher daemon
build/tools/analyze/crashomon-analyze  # CLI symbolication
```

### Optional build flags

```bash
# Enable sanitizers (pick one; do not combine ASan + TSan)
cmake -B build-asan -DENABLE_ASAN=ON && cmake --build build-asan
cmake -B build-tsan -DENABLE_TSAN=ON && cmake --build build-tsan
cmake -B build-ubsan -DENABLE_UBSAN=ON && cmake --build build-ubsan

# Enable microbenchmarks
cmake -B build -DENABLE_BENCHMARKS=ON && cmake --build build

# Static analysis (requires clang-tidy)
cmake -B build-tidy -DENABLE_CLANG_TIDY=ON && cmake --build build-tidy
```

---

## Installation on a Linux Target

### 1. Copy binaries

```bash
install -m 755 build/lib/libcrashomon.so         /usr/lib/
install -m 755 build/daemon/crashomon-watcherd   /usr/libexec/crashomon/
# crashpad_handler lives next to the daemon:
install -m 755 $(find build -name crashpad_handler) /usr/libexec/crashomon/
```

### 2. Install and start the watcher daemon

```bash
cp systemd/crashomon-watcherd.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now crashomon-watcherd

# Verify it is running:
systemctl status crashomon-watcherd
```

The watcher writes tombstones to journald. View them with:

```bash
journalctl -u crashomon-watcherd -f
```

### 3. Enable crash monitoring for a service

The easiest way is a systemd drop-in. No code changes needed.

```bash
mkdir -p /etc/systemd/system/my-service.service.d/
cp systemd/example-app.service.d/crashomon.conf \
   /etc/systemd/system/my-service.service.d/

systemctl daemon-reload
systemctl restart my-service
```

The drop-in sets three environment variables that the service (and all children) inherit:

```ini
[Service]
Environment=LD_PRELOAD=/usr/lib/libcrashomon.so
Environment=CRASHOMON_DB_PATH=/var/crashomon
Environment=CRASHOMON_HANDLER_PATH=/usr/libexec/crashomon/crashpad_handler
```

All dynamically-linked processes in the service tree are automatically monitored — no recompilation required.

---

## Usage

### Zero-code integration (LD_PRELOAD)

```bash
LD_PRELOAD=/usr/lib/libcrashomon.so \
  CRASHOMON_DB_PATH=/var/crashomon \
  CRASHOMON_HANDLER_PATH=/usr/libexec/crashomon/crashpad_handler \
  ./my_program
```

When `my_program` crashes, a minidump appears in `/var/crashomon/` and the watcher logs a tombstone to journald within milliseconds.

### Explicit linking (optional annotations)

Link with `-lcrashomon` and call the C API to attach context to crash reports:

```c
#include "crashomon.h"

int main(void) {
    CrashomonConfig cfg = {
        .db_path      = "/var/crashomon",
        .handler_path = "/usr/libexec/crashomon/crashpad_handler",
    };
    crashomon_init(&cfg);

    crashomon_set_tag("version", "1.2.3");
    crashomon_set_tag("build",   "release");
    crashomon_add_breadcrumb("loaded config file");
    crashomon_add_breadcrumb("connected to database");

    // ... your program ...

    crashomon_shutdown();
    return 0;
}
```

Set an abort message before calling `abort()`:

```c
crashomon_set_abort_message("invariant violated: count >= 0");
abort();
```

---

## Watcher Daemon: CLI Reference

```
crashomon-watcherd [OPTIONS]

  --db-path=PATH    Directory to watch for new .dmp files.
                    Default: $CRASHOMON_DB_PATH or /var/crashomon

  --max-size=SIZE   Total .dmp size budget (e.g. 100M, 500K, 1G).
                    Oldest files are deleted when exceeded. 0 = unlimited.

  --max-age=AGE     Per-file age limit (e.g. 7d, 24h, 3600s).
                    Files older than this are deleted. 0 = unlimited.
```

---

## Symbol Store: crashomon-syms

The symbol store holds Breakpad `.sym` files extracted from debug binaries. It must be populated before symbolication can produce function names and line numbers.

```bash
# Ingest a single debug binary
crashomon-syms add --store /var/crashomon/symbols \
    --dump-syms build/breakpad-src/src/tools/linux/dump_syms/dump_syms \
    ./my_binary

# Ingest all ELF binaries recursively (e.g. after a full build)
crashomon-syms add --store /var/crashomon/symbols --recursive ./build/

# List what is stored
crashomon-syms list --store /var/crashomon/symbols

# Remove old versions, keeping the 3 most recent per module
crashomon-syms prune --store /var/crashomon/symbols --keep 3
```

Store layout (Breakpad-standard, auto-resolved from minidump build ID):

```
/var/crashomon/symbols/
  my_binary/
    A1B2C3D4E5F60000/   ← build ID
      my_binary.sym
  libfoo.so/
    ...
```

### CI/CD symbol upload

After each build, upload symbols so crash reports are always symbolicated:

```bash
# Via CLI tool (from build machine)
crashomon-syms add --store /symbols --recursive ./build/

# Via REST API (from CI runner, targeting a remote crashomon-web)
curl -F binary=@./build/my_binary http://crashomon-web:5000/api/symbols/upload
```

---

## Offline Analysis: crashomon-analyze

Symbolicate a minidump or annotate a pasted tombstone from the developer machine:

```bash
# Symbolicate a minidump
crashomon-analyze /path/to/crash.dmp \
    --symbol-store /var/crashomon/symbols

# Annotate a tombstone text file (already has raw addresses)
crashomon-analyze --tombstone /path/to/tombstone.txt \
    --symbol-store /var/crashomon/symbols
```

Output is a symbolicated report with function names, file names, and line numbers substituted for raw program counter values.

---

## Web UI: crashomon-web

The web UI lets you upload minidumps and browse crash history from a browser.

### Start the server

```bash
# Development
uv run flask --app web.app:create_app run

# Production (gunicorn)
uv run gunicorn "web.app:create_app()" -b 0.0.0.0:5000
```

Environment variables:

| Variable | Default | Description |
|---|---|---|
| `CRASHOMON_SYMBOL_STORE` | `/var/crashomon/symbols` | Symbol store root |
| `CRASHOMON_DB` | `/var/crashomon/crashes.db` | SQLite crash history |
| `SECRET_KEY` | `dev-only-key` | Flask session key — **change in production** |

### REST API (CI/CD)

```bash
# Upload a minidump and get back a crash ID
curl -F minidump=@./crash.dmp http://localhost:5000/api/crashes/upload

# Upload a .sym file directly
curl -F sym=@./my_binary.sym http://localhost:5000/api/symbols/upload

# Upload a debug binary (server runs dump_syms)
curl -F binary=@./my_binary http://localhost:5000/api/symbols/upload

# Delete a symbol version
curl -X DELETE http://localhost:5000/symbols/my_binary/A1B2C3D4E5F60000
```

---

## Working on the Codebase

### Directory layout

```
lib/            libcrashomon.so — C++20 implementation, C-compatible public header
daemon/         crashomon-watcherd — inotify watcher, tombstone formatter, disk manager
tools/
  analyze/      crashomon-analyze — CLI symbolication (C++20)
  syms/         crashomon-syms — symbol store management (Python)
web/            crashomon-web — Flask UI + REST API (Python)
  tests/        pytest tests for the web layer
cmake/          CMake helper modules (cmake/breakpad.cmake)
bench/          Microbenchmarks (Google Benchmark, opt-in)
test/           C/C++ unit tests (GoogleTest) + integration scripts
examples/       Example crasher programs (segfault, abort, multithread)
systemd/        systemd unit files + drop-in snippets
```

### Code standards

- **C++20**, no exceptions (`-fno-exceptions`), no `try`/`catch`/`throw`
- Error handling via `absl::Status` / `absl::StatusOr<T>` (internal only — never in public headers)
- Public API (`lib/crashomon.h`) uses only C types — consumers need no extra dependencies
- `-Wall -Wextra -Wpedantic -Werror` on all project targets
- Google C++ Style Guide + [Abseil Tips of the Week](https://abseil.io/tips/)
- Python: ruff, line length 100, Python 3.11+

### Formatting

```bash
# C/C++
clang-format --dry-run --Werror \
  $(find lib daemon tools -name '*.c' -o -name '*.cpp' -o -name '*.h')

# Fix in place
clang-format -i \
  $(find lib daemon tools -name '*.c' -o -name '*.cpp' -o -name '*.h')

# Python
uv run ruff check web/ tools/syms/
uv run ruff format --check web/ tools/syms/
```

---

## Running the Tests

### C/C++ unit tests

```bash
# Build and run all tests
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure

# Run a specific test suite by name
ctest --test-dir build -R MinidumpReader
ctest --test-dir build -R TombstoneFormatter
```

### Python tests (web layer)

```bash
uv run pytest web/tests/ -v
```

### Fixture-based tests (requires running crashers)

The `test_minidump_reader.cpp` tests need real `.dmp` files generated from the example crashers. Run once to populate `test/fixtures/`:

```bash
# Generate fixtures (requires a built project)
test/gen_fixtures.sh build

# Then re-run tests — fixture tests will no longer be skipped
ctest --test-dir build -R MinidumpReader
```

### Integration test

End-to-end test of the full pipeline: capture → analyze → syms:

```bash
test/integration_test.sh build
```

### Sanitizer builds

```bash
cmake -B build-ubsan -DENABLE_UBSAN=ON
cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure

cmake -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Do not combine ASan and TSan in the same build.

---

## Reading crash tombstones

Tombstones are written to journald by the watcher. On a device:

```bash
# Live stream
journalctl -u crashomon-watcherd -f

# Search for crashes from a specific process
journalctl -u crashomon-watcherd -g "my_service"

# Show all crashes since yesterday
journalctl -u crashomon-watcherd --since yesterday
```

A tombstone looks like:

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 1234, tid: 1234, name: my_service  >>> my_service <<<
signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0000000000000000
timestamp: 2026-03-28T10:15:30Z

   rax 0000000000000000  rbx 00007fff5fbff8a0  rcx 0000000000000001
   rdx 0000000000000000  rsi 00007fff5fbff8a0  rdi 0000000000000001
   rbp 00007fff5fbff870  rsp 00007fff5fbff850  rip 00005555555551a0

backtrace:
    #00 pc 0x00000000000011a0  /usr/bin/my_service
    #01 pc 0x0000000000000f32  /usr/bin/my_service
    #02 pc 0x000000000002b09f  /usr/lib/libc.so.6

--- --- --- thread 1235 (worker) --- --- ---
    #00 pc 0x000000000000ab12  /usr/lib/libc.so.6
    #01 pc 0x0000000000000c44  /usr/bin/my_service

minidump saved to: /var/crashomon/abcdef12-3456-7890-abcd-ef1234567890.dmp
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

Copy the `.dmp` path to your developer machine and run `crashomon-analyze` (or upload to the web UI) to get symbolicated function names and line numbers.

---

## Design decisions

| Decision | Rationale |
|---|---|
| Out-of-process crash capture (Crashpad) | Handler runs in a healthy process; in-process handlers fail when heap is corrupt |
| No heap allocation in signal path | Async-signal-safe constraint — `libcrashomon.so` itself has no signal handler |
| `LD_PRELOAD` via constructor | Zero code changes in monitored programs; child processes inherit the env var |
| No upload URL in Crashpad | Crash data never leaves the device; analysis is always offline |
| Breakpad symbol store layout | `minidump_stackwalk` auto-resolves the right symbol file by build ID |
| Abseil types internal-only | Public API uses only C types — consumers add no dependencies |
| All deps via CMake FetchContent | Reproducible builds; no system-package version conflicts |
