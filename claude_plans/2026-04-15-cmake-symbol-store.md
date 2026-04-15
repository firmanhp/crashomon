# CMake symbol store: crashomon_store_symbols()

## Context

Developers integrating crashomon via FetchContent or add_subdirectory need a single CMake call —
analogous to `install(TARGETS ...)` — that ingests a freshly linked binary's debug symbols into the
Breakpad symbol store at build time. Without this, CI/CD pipelines must hand-roll `crashomon-syms add`
invocations or bespoke shell scripts.

The store layout (`<root>/<module_name>/<build_id>/`) is content-addressed: symbols from different
build versions never collide, and `rsync -a` merges them cleanly into a shared store. A single
`CRASHOMON_SYMBOL_STORE` CMake cache variable gives CI control over the output path.

The same store directory is consumed by both analysis modes:
- `crashomon-analyze --store <dir> --minidump <file>` — uses the `.sym` file via `minidump_stackwalk`
- `crashomon-analyze --debug-dir <dir> --stdin` — uses the unstripped ELF binary via `eu-addr2line`

An optional `WITH_BINARY` flag enables the second use case by also copying the unstripped build-tree
binary into the store alongside its `.sym` file.

---

## Analysis mode reference

| Mode | Flag | Needs .sym | Needs binary |
|---|---|---|---|
| Minidump + store | `--store --minidump` | ✓ | ✗ |
| Raw tombstone | `--minidump` only | ✗ | ✗ |
| Tombstone stdin + store | `--store --stdin` | ✗ | ✓ (target host path) |
| Tombstone stdin + debug-dir | `--debug-dir --stdin` | ✗ | ✓ (indexed by build ID) |
| Single .sym file | `--symbols --minidump` | ✓ | ✗ |

`build_build_id_index()` in `tools/analyze/symbolizer.py:393` already recurses subdirectories and
indexes by GNU build ID — no Python changes needed.

---

## File layout

```
cmake/store_sym_impl.cmake          (new)    cmake -P helper; runs at build time
cmake/crashomon_symbols.cmake       (new)    defines crashomon_store_symbols() + cache vars
CMakeLists.txt                      (modify) add include() after cmake/breakpad.cmake
examples/CMakeLists.txt             (modify) call crashomon_store_symbols(example_segfault)
test/test_symbol_store.sh           (new)    integration test for the full cmake→analyze pipeline
```

No new CMake targets. No existing targets changed beyond the examples POST_BUILD hook.

---

## Steps

### 1. `cmake/store_sym_impl.cmake`

A cmake `-P` script invoked at build time via `add_custom_command POST_BUILD`. All inputs arrive
as `-D` variables so generator expressions (evaluated by the build backend) can feed into them.

Required `-D` vars: `DUMP_SYMS`, `BINARY`, `STORE`, `TARGET_NAME`, `BUILD_DIR`
Optional `-D` var: `WITH_BINARY` (1 to also copy the ELF, 0 otherwise)

```
1. _tmp = "${BUILD_DIR}/.crashomon_sym_${TARGET_NAME}.sym.tmp"
   One temp file per target name → parallel builds never collide on the filesystem.

2. execute_process(
     COMMAND "${DUMP_SYMS}" "${BINARY}"
     OUTPUT_FILE "${_tmp}"      ← stream to disk; avoid CMake-string-buffering large .sym files
     RESULT_VARIABLE _rc
     ERROR_VARIABLE  _err
   )
   On failure: file(REMOVE "${_tmp}"), message(FATAL_ERROR ...)

3. file(STRINGS "${_tmp}" _header LIMIT_COUNT 1)
   Parse: "MODULE <os> <arch> <build_id> <module_name>"
   string(REGEX MATCH "^MODULE [^ ]+ [^ ]+ ([^ ]+) (.+)" _ "${_header}")
   _build_id    = CMAKE_MATCH_1
   _module_name = CMAKE_MATCH_2
   On missing match: file(REMOVE "${_tmp}"), message(FATAL_ERROR ...)

4. _dest_dir = "${STORE}/${_module_name}/${_build_id}"
   file(MAKE_DIRECTORY "${_dest_dir}")
   file(RENAME "${_tmp}" "${_dest_dir}/${_module_name}.sym")
     ↑ atomic on Linux — a reader never sees a partial .sym file
   message(STATUS "  symbols: ${_module_name}/${_build_id}/${_module_name}.sym")

5. if(WITH_BINARY)
     file(COPY_FILE "${BINARY}" "${_dest_dir}/${_module_name}")
       ↑ COPY_FILE requires CMake 3.21 — already the project minimum
       ↑ BINARY is the unstripped build-tree binary (POST_BUILD fires before install stripping)
     message(STATUS "  binary:  ${_module_name}/${_build_id}/${_module_name}")
   endif()
```

### 2. `cmake/crashomon_symbols.cmake`

Module header comment documents the store layout and both analysis commands.

```cmake
# Declare the cache variable here (not in CMakeLists.txt) so it is available
# even when crashomon is consumed as a subproject.
set(CRASHOMON_SYMBOL_STORE "${CMAKE_BINARY_DIR}/symbols" CACHE PATH
  "Root directory for the Breakpad symbol store populated by crashomon_store_symbols()")

# Resolve path relative to this .cmake file, not to CMAKE_SOURCE_DIR.
# CMAKE_CURRENT_LIST_DIR is correct for both top-level and FetchContent use.
set(_CRASHOMON_STORE_SYM_IMPL
  "${CMAKE_CURRENT_LIST_DIR}/store_sym_impl.cmake"
  CACHE INTERNAL "")

function(crashomon_store_symbols target)
  # WITH_BINARY: also copy the unstripped ELF (enables --debug-dir --stdin mode)
  # STORE <dir>: override CRASHOMON_SYMBOL_STORE for this target only
  cmake_parse_arguments(PARSE_ARGV 1 ARG "WITH_BINARY" "STORE" "")

  if(ARG_STORE)
    set(_store "${ARG_STORE}")
  else()
    set(_store "${CRASHOMON_SYMBOL_STORE}")
  endif()

  if(ARG_WITH_BINARY)
    set(_with_binary 1)
  else()
    set(_with_binary 0)
  endif()

  if(NOT TARGET breakpad_dump_syms)
    message(WARNING
      "crashomon_store_symbols: breakpad_dump_syms not found; "
      "skipping symbol ingestion for '${target}'")
    return()
  endif()

  add_custom_command(
    TARGET "${target}"
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DDUMP_SYMS=$<TARGET_FILE:breakpad_dump_syms>"
      "-DBINARY=$<TARGET_FILE:${target}>"
      "-DSTORE=${_store}"
      "-DTARGET_NAME=${target}"
      "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DWITH_BINARY=${_with_binary}"
      -P "${_CRASHOMON_STORE_SYM_IMPL}"
    COMMENT "Storing Breakpad symbols for ${target} → ${_store}"
    VERBATIM
  )
endfunction()
```

Key points:
- `PARSE_ARGV 1` skips the `target` positional arg during keyword parsing
- `CMAKE_CURRENT_LIST_DIR` (not `PROJECT_SOURCE_DIR`) resolves correctly in FetchContent
- `CACHE INTERNAL ""` on `_CRASHOMON_STORE_SYM_IMPL` hides it from cmake-gui

### 3. `CMakeLists.txt`

After line 297 (`include(cmake/breakpad.cmake)`), add one line:

```cmake
include(cmake/crashomon_symbols.cmake)
```

The `CRASHOMON_SYMBOL_STORE` cache variable is declared inside the module, not here.

### 4. `examples/CMakeLists.txt`

After the three `add_executable` / `set_target_properties` blocks, add:

```cmake
# Store debug symbols for the segfault example so integration tests can exercise
# the full cmake-build → crashomon-analyze pipeline without a manual syms add step.
# Guard with COMMAND check so this works when crashomon is embedded as a subproject
# and the consumer has not included crashomon_symbols.cmake.
if(COMMAND crashomon_store_symbols)
  crashomon_store_symbols(example_segfault)
endif()
```

`example_segfault` is the segfault binary used by `demo_symbolicated.sh` and
integration tests. It has the full call chain (`WriteField → main`) that makes
output validation meaningful.

### 5. `test/test_symbol_store.sh`

New bash script following the `log_pass`/`log_fail` pattern of `integration_test.sh`
and `test_run_analyze.sh`. Takes an optional `BUILD_DIR` argument (default: `./build`).

**Structure:**

```
Section 1: Build
  cmake -B BUILD_DIR -DENABLE_TESTS=ON
  cmake --build BUILD_DIR --target example_segfault
  (POST_BUILD fires → symbols land in BUILD_DIR/symbols/)

Section 2: Store layout verification
  PASS/FAIL: STORE_DIR (BUILD_DIR/symbols) is a directory
  PASS/FAIL: exactly one subdirectory under crashomon-example-segfault/
  PASS/FAIL: <module>/<build_id>/<module>.sym file exists
  PASS/FAIL: first line matches "^MODULE Linux " (validates dump_syms ran correctly)
  PASS/FAIL: crashomon-syms list --store STORE_DIR shows ≥1 entry

Section 3: Crash capture (requires watcherd + libcrashomon)
  Start watcherd with --db-path=WORK_DIR/crashdb --socket-path=WORK_DIR/handler.sock
  LD_PRELOAD=libcrashomon.so CRASHOMON_SOCKET_PATH=... run crashomon-example-segfault
  Poll WORK_DIR/crashdb/pending/ for *.dmp (up to 5 s)
  PASS/FAIL: .dmp captured and is non-empty

Section 4: crashomon-analyze against cmake-generated store
  crashomon-analyze \
    --store=STORE_DIR \
    --minidump=<captured.dmp> \
    --stackwalk-binary=BUILD_DIR/.../minidump_stackwalk
  PASS/FAIL: exit 0
  PASS/FAIL: output contains "WriteField"   (frame 0 — the crash site)
  PASS/FAIL: output contains "main"         (outer frame always present)
  PASS/FAIL: output contains "SIGSEGV"      (signal line)
```

Section 3 and 4 are skipped with `echo "  SKIP: ..."` (not `FAIL`) when watcherd
or libcrashomon.so are not present — matching the existing integration test convention.

The script exits 1 if `FAIL > 0`, 0 otherwise. It is standalone (not wired into
CTest) — same as `integration_test.sh` and `test_run_analyze.sh`.

---

## Usage (caller side)

```cmake
# Primary use case — .sym only, enables --store --minidump:
add_executable(my_daemon src/main.cpp)
target_link_libraries(my_daemon PRIVATE crashomon_client)
crashomon_store_symbols(my_daemon)
# → build/symbols/my_daemon/<build_id>/my_daemon.sym

# WITH_BINARY — also enables --debug-dir --stdin for tombstone text:
crashomon_store_symbols(my_daemon WITH_BINARY)
# → build/symbols/my_daemon/<build_id>/my_daemon.sym
# → build/symbols/my_daemon/<build_id>/my_daemon       (unstripped ELF)

# Override store at configure time (CI):
# cmake -B build -DCRASOMON_SYMBOL_STORE=/srv/crashomon/symbols
```

Analysis against the store:
```bash
# Minidump crash:
crashomon-analyze --store build/symbols --minidump crash.dmp \
  --stackwalk-binary build/minidump_stackwalk

# Tombstone text from journalctl (requires WITH_BINARY):
crashomon-analyze --debug-dir build/symbols --stdin < tombstone.txt
```

CI consolidation: `rsync -a build/symbols/ /shared/store/` merges safely across build versions —
no two builds share a `<build_id>`, so artifacts never overwrite each other.

---

## Verification

**Primary — run the integration test script (exercises all five steps at once):**
```bash
cmake -B build -DENABLE_TESTS=ON
test/test_symbol_store.sh build
# Expected: all checks PASS, exit 0
```

**Secondary — manual spot-checks:**

1. Store layout after build:
   ```bash
   find build/symbols -type f
   # build/symbols/crashomon-example-segfault/<hex>/crashomon-example-segfault.sym
   ```

2. MODULE line format:
   ```bash
   head -1 build/symbols/crashomon-example-segfault/*/crashomon-example-segfault.sym
   # MODULE Linux x86_64 <build_id> crashomon-example-segfault
   ```

3. crashomon-syms list round-trip:
   ```bash
   tools/syms/crashomon-syms list --store build/symbols
   # crashomon-example-segfault/<build_id>  (N KB)
   ```

4. Parallel build safety (temp files must not collide):
   ```bash
   cmake --build build --parallel
   ```

5. WITH_BINARY produces readable ELF:
   ```bash
   # (after calling crashomon_store_symbols(example_segfault WITH_BINARY))
   file build/symbols/crashomon-example-segfault/*/crashomon-example-segfault
   # ELF 64-bit LSB pie executable, ... not stripped
   ```

6. Confirmed symbolicated output contains expected call chain (also validated by the test script):
   ```
   WriteField    [segfault.cpp : 19]
   UpdateRecord  [segfault.cpp : 25]
   ...
   main          [segfault.cpp : 55]
   ```
