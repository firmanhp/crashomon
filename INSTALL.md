# Crashomon — Build, Deploy, and Integration

## Prerequisites

### Build machine

| Tool | Version | Notes |
|---|---|---|
| CMake | ≥ 3.25 | |
| C++ compiler | GCC ≥ 13 or Clang ≥ 16 | Must support C++20 |
| Python | ≥ 3.11 | For `crashomon-syms` and `crashomon-web` |
| `uv` | any | Python dependency management (`pip install uv`) |
| `cargo` | any stable | Builds `minidump-stackwalk` ([install via rustup](https://rustup.rs/)) |
| `git` | any | FetchContent clones dependencies |
| `ninja` or `make` | any | CMake build backend |

All C/C++ dependencies (Crashpad, Breakpad, Abseil, GoogleTest, Google Benchmark, zlib) are fetched automatically by CMake — **no system-level installs required** for those. `cargo` is the only additional build-time requirement.

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
# Configure + build all C/C++ targets
cmake -B build
cmake --build build -j$(nproc)

# Install Python deps (web UI + syms script)
uv sync
```

This produces:

```
build/lib/libcrashomon.so                              # LD_PRELOAD library
build/lib/libcrashomon.a                               # static library
build/daemon/crashomon-watcherd                        # watcher daemon (also hosts crash handler)
build/rust_minidump_target/release/minidump-stackwalk  # symbolication engine (statically linked)
tools/analyze/crashomon-analyze                        # CLI symbolication (Python script, no build step)
tools/syms/crashomon-syms                              # symbol store management (Python script, no build step)
```

`minidump-stackwalk` is built from the [rust-minidump](https://github.com/rust-minidump/rust-minidump) crate with `RUSTFLAGS=-C target-feature=+crt-static`, producing a fully statically linked binary with no glibc version requirement. It can be copied to any x86-64 Linux target without installing runtime libraries.

### Optional build flags

```bash
# Enable sanitizers (pick one; do not combine ASan + TSan)
cmake -B build-asan  -DENABLE_ASAN=ON  && cmake --build build-asan
cmake -B build-tsan  -DENABLE_TSAN=ON  && cmake --build build-tsan
cmake -B build-ubsan -DENABLE_UBSAN=ON && cmake --build build-ubsan

# Enable microbenchmarks
cmake -B build -DENABLE_BENCHMARKS=ON && cmake --build build

# Static analysis (requires clang-tidy)
cmake -B build-tidy -DENABLE_CLANG_TIDY=ON && cmake --build build-tidy
```

---

## Deployment on a Linux target

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

# Verify:
systemctl status crashomon-watcherd
journalctl -u crashomon-watcherd -f
```

### 3. Enable crash monitoring for a service

The easiest path is a systemd drop-in — no code changes, no recompilation:

```bash
mkdir -p /etc/systemd/system/my-service.service.d/
cp systemd/example-app.service.d/crashomon.conf \
   /etc/systemd/system/my-service.service.d/
systemctl daemon-reload
systemctl restart my-service
```

The drop-in adds two environment variables that the service and all its children inherit:

```ini
[Service]
Environment=LD_PRELOAD=/usr/lib/libcrashomon.so
Environment=CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock
```

---

## Explicit linking (optional)

Use explicit linking when you want to attach tags or an abort message to crash reports.

Link with `-lcrashomon` and call the C API:

```c
#include "crashomon.h"

int main(void) {
    CrashomonConfig cfg = {
        .socket_path = "/run/crashomon/handler.sock",
    };
    crashomon_init(&cfg);

    crashomon_set_tag("version", "1.2.3");
    crashomon_set_tag("env",     "production");

    /* ... your program ... */

    crashomon_shutdown();
    return 0;
}
```

Set an abort message before calling `abort()`:

```c
crashomon_set_abort_message("invariant violated: count >= 0");
abort();
```

`crashomon_init()` is not needed when using LD_PRELOAD — the library constructor handles initialization automatically.

---

## Watcher daemon: CLI reference

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

  --export-path=PATH   Directory to copy each new minidump into on arrival.
                       Files are named: {process}_{build_id8}_{YYYYMMDDHHmmss}.crashdump
                       Pruned by the same --max-size/--max-age limits as the db.
                       Default: $CRASHOMON_EXPORT_PATH or disabled.
```

---

## Symbol store

The symbol store holds Breakpad `.sym` files extracted from debug binaries. It must be populated before symbolication can produce function names and line numbers.

Store layout (Breakpad-standard, content-addressed by build ID):

```
/var/crashomon/symbols/
  my_binary/
    A1B2C3D4E5F60000/        ← build ID — different builds never collide
      my_binary.sym
  libfoo.so/
    7B3C1A9D.../
      libfoo.so.sym
```

### CMake integration — `crashomon_store_symbols()`

If your project uses CMake, symbols are stored automatically every time the binary is linked:

```cmake
include(FetchContent)
FetchContent_Declare(crashomon
    GIT_REPOSITORY https://github.com/firmanhp/crashomon.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(crashomon)

add_executable(my_daemon src/main.cpp)
target_link_libraries(my_daemon PRIVATE crashomon_client)

# Store .sym automatically on every build:
crashomon_store_symbols(my_daemon)
```

`CRASHOMON_SYMBOL_STORE` controls the output directory (default: `<build>/symbols/`). Override at configure time for CI/CD:

```bash
cmake -B build -DCRASOMON_SYMBOL_STORE=/srv/crashomon/symbols
cmake --build build

# Merge multiple build machines into a shared store safely — each
# build ID subdirectory is unique and never overwrites another:
rsync -a build/symbols/ deploy-host:/srv/crashomon/symbols/
```

### `crashomon-syms` — manual and CLI management

Use `crashomon-syms` for everything outside the CMake graph:

```bash
# Ingest a single debug binary
crashomon-syms add --store /var/crashomon/symbols ./my_binary

# Ingest all ELF binaries under a directory (sysroot, build output, etc.)
crashomon-syms add --store /var/crashomon/symbols --recursive ./build/

# List what is stored
crashomon-syms list --store /var/crashomon/symbols

# Remove old versions, keeping the 3 most recent per module
crashomon-syms prune --store /var/crashomon/symbols --keep 3
```

Use cases for `crashomon-syms` over the CMake function:
- Third-party and OS libraries (`libc.so`, vendor SDKs, firmware blobs)
- Non-CMake build systems (Makefiles, Bazel, Meson, Autotools)
- Retroactive ingestion of binaries built before crashomon was added
- Store inspection and pruning
- `crashomon-web` backend (invoked when symbols are uploaded through the browser)

### CI/CD symbol upload

```bash
# Via CMake (preferred when you control the build):
cmake -B build -DCRASOMON_SYMBOL_STORE=/srv/symbols && cmake --build build

# Via CLI (third-party libs, non-CMake projects, post-deployment):
crashomon-syms add --store /srv/symbols --recursive ./build/

# Via REST API (CI runner uploading to a remote crashomon-web):
curl -F binary=@./build/my_binary http://crashomon-web:5000/api/symbols/upload
```

### Sysroot symbols

To resolve stack frames from system libraries (libc, libstdc++, etc.),
extract their symbols into the same store.

#### Option A: CLI (one-time ingestion)

```bash
crashomon-syms add --store symbols/ --dump-syms ./dump_syms \
    --sysroot /path/to/sysroot
```

#### Option B: Lazy (no setup required)

```bash
crashomon-analyze \
    --store symbols/ \
    --sysroot /path/to/sysroot \
    --minidump crash.dmp \
    --dump-syms-binary ./dump_syms
```

Symbols are extracted on-the-fly and cached in the store for subsequent runs.

---

## Offline analysis: `crashomon-analyze`

Symbolicate a minidump or annotate a pasted tombstone from a developer machine.

| Mode | When to use |
|---|---|
| `--store --minidump` | You have a `.dmp` file and a symbol store |
| `--symbols --minidump` | Single `.sym` file, no full store |
| `--minidump` only | Raw unsymbolicated tombstone — no symbols needed |

```bash
# Symbolicate a minidump against a symbol store (auto-matches by build ID)
crashomon-analyze --store=/var/crashomon/symbols --minidump=crash.dmp

# Use a single explicit .sym file
crashomon-analyze --symbols=my_service.sym --minidump=crash.dmp

# Raw unsymbolicated tombstone from a minidump (no symbol store needed)
crashomon-analyze --minidump=crash.dmp

# Override tool path (default: searches PATH, then libexec/crashomon/)
crashomon-analyze --store=... --minidump=... \
    --stackwalk-binary=/path/to/minidump-stackwalk
```

---

## Web UI: `crashomon-web`

See [web/README.md](web/README.md) for setup and development details.

### Start the server

```bash
# Development
uv run flask --app web.app:create_app run --debug

# Production (gunicorn)
uv run gunicorn "web.app:create_app()" -b 0.0.0.0:5000
```

Environment variables:

| Variable | Default | Description |
|---|---|---|
| `CRASHOMON_SYMBOL_STORE` | `/var/crashomon/symbols` | Symbol store root |
| `CRASHOMON_DB` | `/var/crashomon/crashes.db` | SQLite crash history |
| `SECRET_KEY` | `dev-only-key` | Flask session key — **change in production** |

### REST API

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

## Cross-compilation

This section shows how to build crashomon for a device with a different CPU architecture (e.g. aarch64) from an x86-64 host.

### Step 1: Build `dump_syms` for the host

`dump_syms` is the Breakpad tool that extracts DWARF debug info from ELF binaries into `.sym` files. It reads the binaries you built — it runs on the **build machine**, not the target device. Building it with a cross-compiler produces a target-architecture binary that cannot execute on the host, so it must be built separately with the host compiler before the cross-compilation step.

```bash
cmake -B _dump_syms_build -S cmake/dump_syms_host/
cmake --build _dump_syms_build -j$(nproc)
# Produces: _dump_syms_build/dump_syms
```

This only needs to be done once per host machine. The binary links zlib and libstdc++ statically and has no runtime dependencies beyond libc, so it can be copied to other machines or cached in CI without reinstalling libraries.

### Step 2: Write a toolchain file

```cmake
# toolchain-aarch64.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_PREFIX /opt/my-sdk/bin/aarch64-linux-gnu-)
set(CMAKE_C_COMPILER   ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_PREFIX}g++)
set(CMAKE_AR           ${CROSS_PREFIX}ar)
set(CMAKE_STRIP        ${CROSS_PREFIX}strip)

set(CMAKE_SYSROOT /opt/my-sdk/sysroot)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

For a Yocto SDK, source the `environment-setup-*` script first and reference its variables:

```cmake
set(CMAKE_SYSROOT $ENV{OECORE_TARGET_SYSROOT})
set(CMAKE_C_COMPILER $ENV{CC})
set(CMAKE_CXX_COMPILER $ENV{CXX})
```

For Buildroot, use `output/host/bin/<tuple>-gcc` and `output/host/<tuple>/sysroot`.

### Step 3: Build crashomon for the target

Pass the `dump_syms` binary from Step 1 via `CRASHOMON_DUMP_SYMS_EXECUTABLE`:

```bash
cmake -B build-cross \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
cmake --build build-cross -j$(nproc) \
    --target crashomon_client crashomon_watcherd
```

FetchContent builds all C/C++ dependencies (Crashpad, Breakpad, Abseil, zlib) for the target. The first build takes longer; subsequent rebuilds are fast.

`CRASHOMON_DUMP_SYMS_EXECUTABLE` is required whenever `crashomon_store_symbols()` is called. Configure will fail with an error if the function is used and the variable is not set.

### Step 4: Option A — LD_PRELOAD (no code changes)

Copy to the target and install the systemd unit as described in the [Deployment](#deployment-on-a-linux-target) section above, using the cross-compiled binaries.

### Step 4: Option B — FetchContent (build alongside your project)

```cmake
include(FetchContent)
FetchContent_Declare(crashomon
    GIT_REPOSITORY https://github.com/firmanhp/crashomon.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(crashomon)

target_link_libraries(my_app PRIVATE crashomon_client)
crashomon_store_symbols(my_app)
```

```bash
# Build dump_syms for the host first (see Step 1 above), then:
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" \
    -DCRASOMON_SYMBOL_STORE=/srv/crashomon/symbols
cmake --build build -j$(nproc)
```

### Step 4: Option C — Install and find as a pre-built library

```bash
cmake --install build-cross --prefix /opt/my-sdk/sysroot/usr \
    --component crashomon_client
```

Then in your project:

```cmake
find_path(CRASHOMON_INCLUDE_DIR crashomon.h
    PATHS ${CMAKE_SYSROOT}/usr/include NO_DEFAULT_PATH)
find_library(CRASHOMON_LIB crashomon
    PATHS ${CMAKE_SYSROOT}/usr/lib NO_DEFAULT_PATH)

target_include_directories(my_app PRIVATE ${CRASHOMON_INCLUDE_DIR})
target_link_libraries(my_app PRIVATE ${CRASHOMON_LIB})
```

### Fully static binaries

Useful when the target has a minimal rootfs. Requires static versions of all system libraries in the sysroot (musl-based toolchains work best):

```bash
cmake -B build-cross \
    -DCMAKE_TOOLCHAIN_FILE=toolchain-aarch64.cmake \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"
```
