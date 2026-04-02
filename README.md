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
| `crashomon-watcherd` | C++20 | Crashpad handler host, inotify watcher, tombstone formatter, disk manager |
| `crashomon-analyze` | Python 3.11 | CLI symbolication tool |
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
build/daemon/crashomon-watcherd    # watcher daemon (also hosts crash handler)
tools/analyze/crashomon-analyze    # CLI symbolication (Python script, no build step needed)
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

The drop-in sets two environment variables that the service (and all children) inherit:

```ini
[Service]
Environment=LD_PRELOAD=/usr/lib/libcrashomon.so
Environment=CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock
```

All dynamically-linked processes in the service tree are automatically monitored — no recompilation required.

---

## Usage

### Zero-code integration (LD_PRELOAD)

Start the watcher daemon first (or rely on systemd):

```bash
crashomon-watcherd --db-path=/var/crashomon --socket-path=/run/crashomon/handler.sock &
```

Then run your program:

```bash
LD_PRELOAD=/usr/lib/libcrashomon.so \
  CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock \
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
        .socket_path  = "/run/crashomon/handler.sock",
    };
    crashomon_init(&cfg);

    crashomon_set_tag("version", "1.2.3");
    crashomon_set_tag("env",     "production");

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

  --db-path=PATH       Directory for the Crashpad database and minidumps.
                       Default: $CRASHOMON_DB_PATH or /var/crashomon

  --socket-path=PATH   Unix domain socket for client crash handler connections.
                       Default: $CRASHOMON_SOCKET_PATH or /run/crashomon/handler.sock

  --max-size=SIZE      Total .dmp size budget (e.g. 100M, 500K, 1G).
                       Oldest files are deleted when exceeded. 0 = unlimited.

  --max-age=AGE        Per-file age limit (e.g. 7d, 24h, 3600s).
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
# Symbolicate a minidump against a symbol store (auto-matches by build ID)
crashomon-analyze --store=/var/crashomon/symbols --minidump=crash.dmp

# Annotate a tombstone piped from stdin
crashomon-analyze --store=/var/crashomon/symbols --stdin < tombstone.txt

# Use a single explicit .sym file
crashomon-analyze --symbols=my_service.sym --minidump=crash.dmp

# Raw unsymbolicated tombstone from a minidump (no symbol store needed)
crashomon-analyze --minidump=crash.dmp

# Override tool paths if not in PATH
crashomon-analyze --store=... --minidump=... \
    --stackwalk-binary=/path/to/minidump_stackwalk \
    --addr2line-binary=/path/to/eu-addr2line
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
tombstone/      shared C++ library: minidump reader + tombstone formatter (used by daemon + tests)
tools/
  analyze/      crashomon-analyze — CLI symbolication (Python 3.11 package)
  syms/         crashomon-syms — symbol store management (Python)
web/            crashomon-web — Flask UI + REST API (Python, imports tools.analyze)
  tests/        pytest tests for the web layer and analyze tool
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
uv run ruff check web/ tools/analyze/ tools/syms/
uv run ruff format --check web/ tools/analyze/ tools/syms/
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

### Python tests (web layer + analyze tool)

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

## Integrating into an Existing CMake Project (Cross-Compilation)

This tutorial shows how to add crash monitoring to an existing CMake project that already uses a custom cross-compiler toolchain (e.g. a Yocto SDK, Buildroot sysroot, or a bare `arm-linux-gnueabihf-` prefix).

### Overview

Crashomon ships two build artifacts relevant to a client project:

| Artifact | What you need it for |
|---|---|
| `libcrashomon.so` / `libcrashomon.a` | Link into your application, or preload via `LD_PRELOAD` |
| `crashomon-watcherd` | Deploy and run on the target device |

The simplest integration is **LD_PRELOAD** — build `libcrashomon.so` for the target, copy it to the device, and add two environment variables to your service. No changes to your CMakeLists.txt required.

If you want crash context (tags, breadcrumbs) or prefer static linking, use explicit linking.

---

### Step 1: Write a toolchain file

CMake cross-compilation is driven by a toolchain file. Create one for your target. This example targets a 64-bit ARMv8 Linux device with a sysroot at `/opt/my-sdk/sysroot`:

```cmake
# toolchain-aarch64.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Path to the cross compiler binaries
set(CROSS_PREFIX /opt/my-sdk/bin/aarch64-linux-gnu-)
set(CMAKE_C_COMPILER   ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_PREFIX}g++)
set(CMAKE_AR           ${CROSS_PREFIX}ar)
set(CMAKE_STRIP        ${CROSS_PREFIX}strip)

# Sysroot — headers and libraries for the target
set(CMAKE_SYSROOT /opt/my-sdk/sysroot)

# Only search the sysroot for libraries and headers, never the host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

Adjust `CROSS_PREFIX` and `CMAKE_SYSROOT` to match your SDK. For a Yocto SDK, source the `environment-setup-*` script first — it sets `CC`, `CXX`, and `OECORE_TARGET_SYSROOT` which you can reference here.

---

### Step 2: Build crashomon for the target

Build crashomon as a standalone project, targeting your device. Only the client library and watcherd need to be cross-compiled.

```bash
# In the crashomon repo root
cmake -B build-cross \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build-cross -j$(nproc) \
    --target crashomon_client crashomon_watcherd
```

This produces:
```
build-cross/lib/libcrashomon.so
build-cross/lib/libcrashomon.a
build-cross/daemon/crashomon-watcherd
```

> **Note:** FetchContent downloads source and builds all dependencies (Crashpad, Breakpad, Abseil, zlib) for the target. The first build takes longer; subsequent rebuilds are fast.

---

### Step 3: Option A — LD_PRELOAD (zero code changes)

This requires no modifications to your project's CMakeLists.txt or source code.

**On the target device:**

```bash
# Copy the shared library
install -m 755 libcrashomon.so /usr/lib/

# Copy and install the watcher
install -m 755 crashomon-watcherd /usr/libexec/crashomon/

# Install and start the systemd unit
cp crashomon-watcherd.service /etc/systemd/system/
systemctl enable --now crashomon-watcherd
```

**Enable crash monitoring for your service** via a systemd drop-in (no rebuild required):

```bash
mkdir -p /etc/systemd/system/my-service.service.d/

cat > /etc/systemd/system/my-service.service.d/crashomon.conf << 'EOF'
[Service]
Environment=LD_PRELOAD=/usr/lib/libcrashomon.so
Environment=CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock
EOF

systemctl daemon-reload
systemctl restart my-service
```

All child processes of `my-service` inherit the environment and are monitored automatically.

---

### Step 3: Option B — Explicit linking (add to your CMakeLists.txt)

Use this approach when you want to call the API to attach tags and breadcrumbs to crash reports, or when you prefer a fully static binary.

#### Install crashomon headers and libraries

After the cross build, install the artifacts to a prefix that your project can find:

```bash
cmake --install build-cross --prefix /opt/my-sdk/sysroot/usr \
    --component crashomon_client
```

Or copy them manually:

```bash
# Headers
install -m 644 lib/crashomon.h /opt/my-sdk/sysroot/usr/include/

# Shared library
install -m 755 build-cross/lib/libcrashomon.so \
    /opt/my-sdk/sysroot/usr/lib/

# Static library
install -m 644 build-cross/lib/libcrashomon.a \
    /opt/my-sdk/sysroot/usr/lib/
```

#### Add crashomon to your CMakeLists.txt

**Option B1 — Find as an installed library (recommended):**

```cmake
# In your project's CMakeLists.txt

# Locate the header and library under the sysroot
find_path(CRASHOMON_INCLUDE_DIR crashomon.h
    PATHS ${CMAKE_SYSROOT}/usr/include
    NO_DEFAULT_PATH
)
find_library(CRASHOMON_LIB crashomon
    PATHS ${CMAKE_SYSROOT}/usr/lib
    NO_DEFAULT_PATH
)

if(NOT CRASHOMON_INCLUDE_DIR OR NOT CRASHOMON_LIB)
    message(FATAL_ERROR "crashomon not found. Run the cross build and install step first.")
endif()

target_include_directories(my_app PRIVATE ${CRASHOMON_INCLUDE_DIR})
target_link_libraries(my_app PRIVATE ${CRASHOMON_LIB})
```

**Option B2 — FetchContent (build alongside your project):**

If you prefer to let CMake build crashomon as part of your project (one cmake invocation):

```cmake
include(FetchContent)

FetchContent_Declare(crashomon
    GIT_REPOSITORY https://github.com/firmanhp/crashomon.git
    GIT_TAG        main   # or a specific tag/commit
)
FetchContent_MakeAvailable(crashomon)

# Link the client library into your target
target_link_libraries(my_app PRIVATE crashomon_client)
```

Pass your toolchain file at configure time and CMake will cross-compile crashomon and all its dependencies automatically:

```bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DFETCHCONTENT_QUIET=OFF
cmake --build build -j$(nproc)
```

#### Initialize in your application

```c
#include "crashomon.h"

int main(void) {
    CrashomonConfig cfg = {
        .socket_path = "/run/crashomon/handler.sock",
    };
    crashomon_init(&cfg);

    crashomon_set_tag("version", "1.2.3");
    crashomon_set_tag("env",     "production");

    /* ... your application ... */

    crashomon_shutdown();
    return 0;
}
```

---

### Step 4: Deploy and verify

Copy the cross-compiled binaries to the target and start the watcher:

```bash
# On the target
crashomon-watcherd \
    --db-path=/var/crashomon \
    --socket-path=/run/crashomon/handler.sock &

# Run your program (with explicit linking or LD_PRELOAD)
LD_PRELOAD=/usr/lib/libcrashomon.so \
  CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock \
  ./my_program

# Trigger a crash and watch for the tombstone
journalctl -u crashomon-watcherd -f
```

You should see a tombstone printed within milliseconds of the crash.

---

### Toolchain-specific notes

**Yocto / OpenEmbedded SDK:**

Source the SDK environment before running CMake — it sets `CC`, `CXX`, `CFLAGS`, `LDFLAGS`, and `OECORE_TARGET_SYSROOT`:

```bash
source /opt/poky/4.0/environment-setup-cortexa72-poky-linux
cmake -B build-cross -DCMAKE_TOOLCHAIN_FILE=toolchain-aarch64.cmake
```

In the toolchain file, reference the SDK variables:
```cmake
set(CMAKE_SYSROOT $ENV{OECORE_TARGET_SYSROOT})
set(CMAKE_C_COMPILER $ENV{CC})
set(CMAKE_CXX_COMPILER $ENV{CXX})
```

**Buildroot:**

Use the `host/bin/<tuple>-gcc` and `host/<tuple>/sysroot` paths from your Buildroot output directory:

```cmake
set(CROSS_PREFIX /path/to/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-)
set(CMAKE_SYSROOT /path/to/buildroot/output/host/aarch64-buildroot-linux-gnu/sysroot)
```

**Static binaries:**

To build a fully static `crashomon-watcherd` (useful when the target has a minimal rootfs):

```bash
cmake -B build-cross \
    -DCMAKE_TOOLCHAIN_FILE=toolchain-aarch64.cmake \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"
```

Note that static linking requires static versions of all system libraries (glibc, libstdc++, libpthread) in the sysroot. Musl-based toolchains (e.g. Alpine, uclibc-ng) are better suited for fully static builds.

---

## Design decisions

| Decision | Rationale |
|---|---|
| Out-of-process crash capture (Crashpad) | Handler runs in a healthy process; in-process handlers fail when heap is corrupt |
| Single `ExceptionHandlerServer` with shared socket | Crashpad's intended model for long-lived handlers (`multiple_clients=true`); eliminates per-client threads and subprocess spawning |
| `SCM_RIGHTS` fd sharing over registration socket | Clients receive the shared socket endpoint from the daemon; no per-process handler spawning (which would fork-bomb under LD_PRELOAD) |
| No heap allocation in signal path | Async-signal-safe constraint — `libcrashomon.so` itself has no signal handler |
| `LD_PRELOAD` via constructor | Zero code changes in monitored programs; child processes inherit the env var |
| No upload URL in Crashpad | Crash data never leaves the device; analysis is always offline |
| Breakpad symbol store layout | `minidump_stackwalk` auto-resolves the right symbol file by build ID |
| Abseil types internal-only | Public API uses only C types — consumers add no dependencies |
| All deps via CMake FetchContent | Reproducible builds; no system-package version conflicts |
