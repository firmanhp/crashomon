# Crashomon — Developer Guide

## Directory layout

```
lib/            libcrashomon.so — C++20 implementation, C-compatible public header
daemon/         crashomon-watcherd — inotify watcher, tombstone formatter, disk manager
tombstone/      shared C++ library: minidump reader + tombstone formatter (used by daemon + tests)
tools/
  analyze/      crashomon-analyze — CLI symbolication (Python 3.11 package)
  syms/         crashomon-syms — symbol store management (Python)
web/            crashomon-web — Flask UI + REST API (Python, imports tools.analyze)
cmake/
  breakpad.cmake            Breakpad CMake targets (processor, stackwalk, dump_syms IMPORTED)
  crashomon_symbols.cmake   crashomon_store_symbols() function
  fetch_breakpad_deps.cmake FetchContent population for zlib, Breakpad, LSS (shared by main build
                            and dump_syms_host/)
  store_sym_impl.cmake      POST_BUILD cmake -P script that runs dump_syms and writes the store
  dump_syms_host/           Standalone CMake project — builds the host dump_syms binary
bench/          Microbenchmarks (Google Benchmark, opt-in via -DENABLE_BENCHMARKS=ON)
test/           C/C++ unit tests (GoogleTest) + integration + smoke test scripts
examples/       Example crasher programs (segfault, abort, multithread)
systemd/        systemd unit files + drop-in snippets
```

---

## Code standards

- **C++20**, no exceptions (`-fno-exceptions`), no `try`/`catch`/`throw`
- Error handling via `absl::Status` / `absl::StatusOr<T>` (internal only — never in public headers)
- Public API (`lib/crashomon.h`) uses only C types — consumers need no extra dependencies
- `-Wall -Wextra -Wpedantic -Werror` on all project targets
- Google C++ Style Guide + [Abseil Tips of the Week](https://abseil.io/tips/)
- Python: ruff, line length 100, Python 3.11+

---

## Formatting

```bash
# C/C++ — check
clang-format --dry-run --Werror \
  $(find lib daemon tombstone tools/analyze -name '*.c' -o -name '*.cpp' -o -name '*.h')

# C/C++ — fix in place
clang-format -i \
  $(find lib daemon tombstone tools/analyze -name '*.c' -o -name '*.cpp' -o -name '*.h')

# Python
uv run ruff check web/ tools/analyze/ tools/syms/
uv run ruff format --check web/ tools/analyze/ tools/syms/
```

---

## Running tests

C++ tests use CTest; Python tests use pytest directly — they are not mixed.

### Build first

`ENABLE_TESTS=ON` adds the `examples/` targets, which call `crashomon_store_symbols()` and therefore require the host `dump_syms` binary. Build it once before configuring:

```bash
# Build dump_syms for the host (one-time)
cmake -B _dump_syms_build -S cmake/dump_syms_host/
cmake --build _dump_syms_build -j$(nproc)

# Configure and build with tests
cmake -B build \
    -DENABLE_TESTS=ON \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
cmake --build build -j$(nproc)
```

### C++ unit tests

```bash
# All C++ tests
ctest --test-dir build --output-on-failure

# Client library only
ctest --test-dir build -R crashomon_client --output-on-failure

# Daemon + tombstone only
ctest --test-dir build -R crashomon_daemon --output-on-failure
```

### Python tests (tools/analyze + web)

```bash
uv run pytest web/tests/ -v
```

### Integration tests

```bash
# Full pipeline: watcherd capture → analyze → syms
test/integration_test.sh build

# CMake symbol store: build → store layout → crashomon-analyze with CMake-generated store
test/test_symbol_store.sh build
```

### Fixture-based tests (require real minidumps)

The `MinidumpReader` tests need real `.dmp` files. Run once to populate `test/fixtures/`:

```bash
test/gen_fixtures.sh build
ctest --test-dir build -R MinidumpReader
```

### dump_syms smoke tests

Standalone script — no running daemon required. Pass the host binary and any debug ELF:

```bash
test/test_dump_syms_smoke.sh _dump_syms_build/dump_syms build/examples/crashomon-example-segfault
```

Checks: exit codes, MODULE line format, build ID encoding, FUNC/FILE record presence, stripped binary handling, self-processing, and error paths.

### Sanitizer builds

```bash
cmake -B build-ubsan \
    -DENABLE_UBSAN=ON -DENABLE_TESTS=ON \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure

cmake -B build-asan \
    -DENABLE_ASAN=ON -DENABLE_TESTS=ON \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
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
