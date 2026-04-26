# Host Toolkit Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the scattered `cmake/dump_syms_host/` + `cmake/rust_minidump.cmake` + manual script copy with a single `cmake/host_toolkit/` project that builds `dump_syms`, `minidump-stackwalk`, and stages `crashomon-analyze` via one target.

**Architecture:** A new standalone CMake project at `cmake/host_toolkit/` builds all three host tools into `<build>/bin/`. The main CMakeLists.txt gains a `CRASHOMON_HOST_TOOLKIT_DIR` variable that auto-derives `CRASHOMON_DUMP_SYMS_EXECUTABLE` and `MINIDUMP_STACKWALK_EXECUTABLE`. Old files (`cmake/dump_syms_host/`, `cmake/rust_minidump.cmake`) are removed.

**Tech Stack:** CMake 3.25+, C++17 (Breakpad), ExternalProject (Cargo/Rust), bash

---

## File Map

| Action | Path |
|---|---|
| Create | `cmake/host_toolkit/CMakeLists.txt` |
| Modify | `CMakeLists.txt` |
| Modify | `cmake/breakpad.cmake` |
| Modify | `cmake/crashomon_symbols.cmake` |
| Modify | `demo/CMakeLists.txt` |
| Modify | `demo/run_demo.sh` |
| Modify | `test/demo_symbolicated.sh` |
| Modify | `DEVELOP.md` |
| Modify | `INSTALL.md` |
| Delete | `cmake/dump_syms_host/CMakeLists.txt` |
| Delete | `cmake/rust_minidump.cmake` |

---

## Task 1: Create `cmake/host_toolkit/CMakeLists.txt`

**Files:**
- Create: `cmake/host_toolkit/CMakeLists.txt`

- [ ] **Step 1: Write the file**

```cmake
cmake_minimum_required(VERSION 3.25)
project(crashomon_host_toolkit C CXX)

# ── Language standards ─────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All executables land in bin/ so CRASHOMON_HOST_TOOLKIT_DIR can point at a
# single flat directory containing dump_syms, minidump-stackwalk, and
# crashomon-analyze.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── Fetch Breakpad, zlib, LSS ─────────────────────────────────────────────────
include("${CMAKE_CURRENT_SOURCE_DIR}/../fetch_breakpad_deps.cmake")

set(BREAKPAD_SRC "${crashomon_breakpad_SOURCE_DIR}/src")

# ── zlib (static) ─────────────────────────────────────────────────────────────
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${crashomon_zlib_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/zlib-build" EXCLUDE_FROM_ALL)
target_compile_options(zlibstatic PRIVATE -w)
set_target_properties(zlibstatic PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/zlib-build"
)

find_package(Threads REQUIRED)

# ── breakpad_libdisasm ─────────────────────────────────────────────────────────
file(GLOB _libdisasm_sources "${BREAKPAD_SRC}/third_party/libdisasm/*.c")
add_library(breakpad_libdisasm STATIC ${_libdisasm_sources})
target_compile_options(breakpad_libdisasm PRIVATE -w)
target_include_directories(breakpad_libdisasm PUBLIC "${BREAKPAD_SRC}/third_party/libdisasm")

# ── breakpad_processor ─────────────────────────────────────────────────────────
file(GLOB _processor_sources "${BREAKPAD_SRC}/processor/*.cc")
list(FILTER _processor_sources EXCLUDE REGEX
  "(_unittest|_selftest|synth_minidump|_test)\\.cc$|minidump_stackwalk\\.cc$|minidump_dump\\.cc$|microdump_stackwalk\\.cc$"
)
add_library(breakpad_processor STATIC
  ${_processor_sources}
  "${BREAKPAD_SRC}/common/linux/scoped_pipe.cc"
  "${BREAKPAD_SRC}/common/linux/scoped_tmpfile.cc"
  "${BREAKPAD_SRC}/common/path_helper.cc"
)
target_compile_options(breakpad_processor PRIVATE -w)
target_compile_definitions(breakpad_processor PRIVATE BPLOG_MINIMUM_SEVERITY=SEVERITY_ERROR)
target_include_directories(breakpad_processor PUBLIC "${BREAKPAD_SRC}")
target_link_libraries(breakpad_processor PRIVATE breakpad_libdisasm Threads::Threads zlibstatic)
add_dependencies(breakpad_processor zlibstatic)

# ── dump_syms ─────────────────────────────────────────────────────────────────
add_executable(dump_syms
  "${BREAKPAD_SRC}/common/dwarf_cfi_to_module.cc"
  "${BREAKPAD_SRC}/common/dwarf_cu_to_module.cc"
  "${BREAKPAD_SRC}/common/dwarf_line_to_module.cc"
  "${BREAKPAD_SRC}/common/dwarf_range_list_handler.cc"
  "${BREAKPAD_SRC}/common/language.cc"
  "${BREAKPAD_SRC}/common/module.cc"
  "${BREAKPAD_SRC}/common/path_helper.cc"
  "${BREAKPAD_SRC}/common/stabs_reader.cc"
  "${BREAKPAD_SRC}/common/stabs_to_module.cc"
  "${BREAKPAD_SRC}/common/dwarf/bytereader.cc"
  "${BREAKPAD_SRC}/common/dwarf/dwarf2diehandler.cc"
  "${BREAKPAD_SRC}/common/dwarf/dwarf2reader.cc"
  "${BREAKPAD_SRC}/common/dwarf/elf_reader.cc"
  "${BREAKPAD_SRC}/common/linux/crc32.cc"
  "${BREAKPAD_SRC}/common/linux/dump_symbols.cc"
  "${BREAKPAD_SRC}/common/linux/elf_symbols_to_module.cc"
  "${BREAKPAD_SRC}/common/linux/elfutils.cc"
  "${BREAKPAD_SRC}/common/linux/file_id.cc"
  "${BREAKPAD_SRC}/common/linux/linux_libc_support.cc"
  "${BREAKPAD_SRC}/common/linux/memory_mapped_file.cc"
  "${BREAKPAD_SRC}/common/linux/safe_readlink.cc"
  "${BREAKPAD_SRC}/tools/linux/dump_syms/dump_syms.cc"
)
target_compile_options(dump_syms PRIVATE -w -DN_UNDF=0)
target_include_directories(dump_syms PRIVATE "${BREAKPAD_SRC}")
target_link_libraries(dump_syms PRIVATE zlibstatic)
target_link_options(dump_syms PRIVATE -static-libstdc++ -static-libgcc)

# ── minidump-stackwalk (via Cargo) ────────────────────────────────────────────
find_program(CARGO cargo
  HINTS "$ENV{HOME}/.cargo/bin"
  DOC "Cargo package manager (install via rustup)")

if(NOT CARGO)
  message(FATAL_ERROR
    "cargo not found. Install Rust via rustup: https://rustup.rs/\n"
    "Then re-run cmake.")
endif()

include(ExternalProject)

get_filename_component(_CARGO_DIR "${CARGO}" DIRECTORY)
find_program(_RUSTC rustc HINTS "${_CARGO_DIR}" "$ENV{HOME}/.cargo/bin" NO_DEFAULT_PATH)
execute_process(
  COMMAND "${_RUSTC}" --print host
  OUTPUT_VARIABLE _RUST_HOST_TRIPLE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
string(TOUPPER  "${_RUST_HOST_TRIPLE}" _RUST_TRIPLE_UPPER)
string(REPLACE  "-" "_" _RUST_TRIPLE_UPPER "${_RUST_TRIPLE_UPPER}")
set(_RUSTFLAGS_VAR "CARGO_TARGET_${_RUST_TRIPLE_UPPER}_RUSTFLAGS")

ExternalProject_Add(rust_minidump_build
  GIT_REPOSITORY https://github.com/rust-minidump/rust-minidump.git
  GIT_TAG        v0.26.1
  GIT_SHALLOW    TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
      "${_RUSTFLAGS_VAR}=-C target-feature=+crt-static"
    ${CARGO} build --release --bin minidump-stackwalk
    --target-dir ${CMAKE_BINARY_DIR}/rust_minidump_target
  BUILD_IN_SOURCE   TRUE
  INSTALL_COMMAND   ""
  BUILD_BYPRODUCTS
    ${CMAKE_BINARY_DIR}/rust_minidump_target/release/minidump-stackwalk
  BUILD_ALWAYS      FALSE
)

unset(_CARGO_DIR)
unset(_RUSTC)
unset(_RUST_HOST_TRIPLE)
unset(_RUST_TRIPLE_UPPER)
unset(_RUSTFLAGS_VAR)

# ── minidump-stackwalk staging ────────────────────────────────────────────────
set(_STACKWALK_SRC "${CMAKE_BINARY_DIR}/rust_minidump_target/release/minidump-stackwalk")
set(_STACKWALK_OUT "${CMAKE_BINARY_DIR}/bin/minidump-stackwalk")

add_custom_command(
  OUTPUT  "${_STACKWALK_OUT}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy "${_STACKWALK_SRC}" "${_STACKWALK_OUT}"
  DEPENDS rust_minidump_build
  VERBATIM
)
add_custom_target(host_toolkit_stage_stackwalk DEPENDS "${_STACKWALK_OUT}")

# ── crashomon-analyze staging ─────────────────────────────────────────────────
set(_ANALYZE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/analyze/crashomon-analyze")
set(_ANALYZE_OUT "${CMAKE_BINARY_DIR}/bin/crashomon-analyze")

add_custom_command(
  OUTPUT  "${_ANALYZE_OUT}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy "${_ANALYZE_SRC}" "${_ANALYZE_OUT}"
  DEPENDS "${_ANALYZE_SRC}"
  VERBATIM
)
add_custom_target(host_toolkit_stage_analyze DEPENDS "${_ANALYZE_OUT}")

# ── host_toolkit aggregator ───────────────────────────────────────────────────
add_custom_target(host_toolkit
  DEPENDS dump_syms host_toolkit_stage_stackwalk host_toolkit_stage_analyze
)

# ── Install ────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS dump_syms RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${_STACKWALK_SRC}" DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${_ANALYZE_SRC}"   DESTINATION ${CMAKE_INSTALL_BINDIR})
```

- [ ] **Step 2: Configure the new project (dump_syms only — skip cargo for speed)**

```bash
cmake -B _host_toolkit -S cmake/host_toolkit/ -Wno-dev 2>&1 | tail -5
```

Expected: `-- Configuring done` and `-- Build files have been written to: .../crashomon/_host_toolkit`

- [ ] **Step 3: Build dump_syms only**

```bash
cmake --build _host_toolkit --target dump_syms -j$(nproc) 2>&1 | tail -5
```

Expected: `[100%] Built target dump_syms`

- [ ] **Step 4: Verify dump_syms binary is in bin/**

```bash
ls -la _host_toolkit/bin/dump_syms && _host_toolkit/bin/dump_syms --version 2>&1 | head -2
```

Expected: file exists, dump_syms outputs usage or version info

- [ ] **Step 5: Commit**

```bash
git add cmake/host_toolkit/CMakeLists.txt
git commit -m "feat(cmake): add cmake/host_toolkit standalone project

Consolidates dump_syms, minidump-stackwalk, and crashomon-analyze into a
single cmake/host_toolkit/ project with a host_toolkit aggregator target.
Outputs all three tools to <build>/bin/.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 2: Update main `CMakeLists.txt`

**Files:**
- Modify: `CMakeLists.txt:72-79` (install options)
- Modify: `CMakeLists.txt:248-250` (rust_minidump include block)

- [ ] **Step 1: Add `CRASHOMON_HOST_TOOLKIT_DIR` block and remove `CRASHOMON_BUILD_ANALYZE`**

In `CMakeLists.txt`, replace the install options block (lines 72–79):

```cmake
option(CRASHOMON_ENABLE_INSTALL   "Generate crashomon install() rules"    ${_CRASHOMON_INSTALL_DEFAULT})
option(CRASHOMON_INSTALL_CLIENT   "Install client library + header"        ON)
option(CRASHOMON_INSTALL_WATCHERD "Install watcherd daemon + dump_syms"    ON)
option(CRASHOMON_INSTALL_ANALYZE  "Install analyze CLI + stackwalk"        OFF)
# Separate build toggle: build the analyze tools (minidump_stackwalk) without
# installing them. Useful when consuming crashomon as a subproject but still
# needing the tool at build time.
option(CRASHOMON_BUILD_ANALYZE    "Build analyze tools without installing" OFF)
```

with:

```cmake
option(CRASHOMON_ENABLE_INSTALL   "Generate crashomon install() rules"    ${_CRASHOMON_INSTALL_DEFAULT})
option(CRASHOMON_INSTALL_CLIENT   "Install client library + header"        ON)
option(CRASHOMON_INSTALL_WATCHERD "Install watcherd daemon + dump_syms"    ON)
option(CRASHOMON_INSTALL_ANALYZE  "Install analyze CLI + stackwalk"        OFF)
```

- [ ] **Step 2: Add `CRASHOMON_HOST_TOOLKIT_DIR` auto-derive block before `include(cmake/breakpad.cmake)`**

In `CMakeLists.txt`, the Breakpad section starts at (approximately) line 244:

```cmake
# Breakpad — minidump-processor library + dump_syms + minidump_stackwalk tools.
# Breakpad uses GN/autotools; we define our own CMake targets (vcpkg-style).
# Source trees populated by fetch_breakpad_deps.cmake above.
include(cmake/breakpad.cmake)
if(CRASHOMON_INSTALL_ANALYZE OR CRASHOMON_BUILD_ANALYZE)
  include(cmake/rust_minidump.cmake)
endif()
include(cmake/crashomon_symbols.cmake)
```

Replace the entire block above with:

```cmake
# Breakpad — minidump-processor library + dump_syms + minidump_stackwalk tools.
# Breakpad uses GN/autotools; we define our own CMake targets (vcpkg-style).
# Source trees populated by fetch_breakpad_deps.cmake above.

# ── Host toolkit integration ───────────────────────────────────────────────────
# Build the host tools once with cmake/host_toolkit/ and point CRASHOMON_HOST_TOOLKIT_DIR
# at the resulting bin/ directory:
#
#   cmake -B _host_toolkit -S cmake/host_toolkit/
#   cmake --build _host_toolkit --target host_toolkit
#   cmake -B build -DCRASHOMON_HOST_TOOLKIT_DIR=$(pwd)/_host_toolkit/bin
#
# CRASHOMON_DUMP_SYMS_EXECUTABLE and MINIDUMP_STACKWALK_EXECUTABLE can also be set
# directly to override individual tools (e.g. for cross-compilation pre-builds).

set(CRASHOMON_HOST_TOOLKIT_DIR "" CACHE PATH
  "Directory of host_toolkit outputs (dump_syms, minidump-stackwalk, crashomon-analyze).")
set(MINIDUMP_STACKWALK_EXECUTABLE "" CACHE FILEPATH
  "Path to minidump-stackwalk binary (auto-derived from CRASHOMON_HOST_TOOLKIT_DIR if set).")

if(CRASHOMON_HOST_TOOLKIT_DIR)
  if(NOT CRASHOMON_DUMP_SYMS_EXECUTABLE)
    set(CRASHOMON_DUMP_SYMS_EXECUTABLE
      "${CRASHOMON_HOST_TOOLKIT_DIR}/dump_syms"
      CACHE FILEPATH "Path to the pre-built host dump_syms binary." FORCE)
  endif()
  if(NOT MINIDUMP_STACKWALK_EXECUTABLE)
    set(MINIDUMP_STACKWALK_EXECUTABLE
      "${CRASHOMON_HOST_TOOLKIT_DIR}/minidump-stackwalk"
      CACHE FILEPATH "Path to the rust-minidump minidump-stackwalk binary." FORCE)
  endif()
endif()

if(CRASHOMON_INSTALL_ANALYZE AND NOT MINIDUMP_STACKWALK_EXECUTABLE)
  message(FATAL_ERROR
    "CRASHOMON_INSTALL_ANALYZE=ON requires MINIDUMP_STACKWALK_EXECUTABLE to be set.\n"
    "Build the host toolkit first:\n"
    "  cmake -B _host_toolkit -S cmake/host_toolkit/\n"
    "  cmake --build _host_toolkit --target host_toolkit\n"
    "Then re-run configure with:\n"
    "  -DCRASHOMON_HOST_TOOLKIT_DIR=<path>/_host_toolkit/bin")
endif()

include(cmake/breakpad.cmake)
include(cmake/crashomon_symbols.cmake)
```

- [ ] **Step 3: Verify main project still configures without CRASHOMON_HOST_TOOLKIT_DIR**

```bash
cmake -B build-check -Wno-dev 2>&1 | tail -5
rm -rf build-check
```

Expected: `-- Configuring done` with no errors

- [ ] **Step 4: Verify main project configures with CRASHOMON_HOST_TOOLKIT_DIR**

```bash
cmake -B build-check -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin" -Wno-dev 2>&1 | tail -5
rm -rf build-check
```

Expected: `-- Configuring done`. CRASHOMON_DUMP_SYMS_EXECUTABLE should be auto-set.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "feat(cmake): add CRASHOMON_HOST_TOOLKIT_DIR; remove CRASHOMON_BUILD_ANALYZE

Replace the CRASHOMON_BUILD_ANALYZE opt-in (which triggered cmake/rust_minidump.cmake
in the main build) with CRASHOMON_HOST_TOOLKIT_DIR. When set, CRASHOMON_DUMP_SYMS_EXECUTABLE
and MINIDUMP_STACKWALK_EXECUTABLE are auto-derived from the host_toolkit bin/ directory.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 3: Update `cmake/breakpad.cmake` and `cmake/crashomon_symbols.cmake` comments

**Files:**
- Modify: `cmake/breakpad.cmake:52-82`
- Modify: `cmake/crashomon_symbols.cmake:67-74`

- [ ] **Step 1: Update the dump_syms comment block in `cmake/breakpad.cmake`**

Replace the comment block at lines 52–68:

```cmake
# ── breakpad_dump_syms ─────────────────────────────────────────────────────────
# CLI tool: extracts DWARF debug info from ELF binaries into Breakpad .sym files.
# Used by crashomon_store_symbols() as a POST_BUILD step.
#
# dump_syms is a host tool — it reads the binaries you built and must run on the
# build machine, not the target. Build it once with the host compiler via the
# standalone sub-project, then pass the result here:
#
#   cmake -B _dump_syms_build -S cmake/dump_syms_host/
#   cmake --build _dump_syms_build
#   cmake -B build -DCRASHOMON_DUMP_SYMS_EXECUTABLE=$(pwd)/_dump_syms_build/dump_syms
#
# For cross-compilation this step is required; for native builds it is only
# needed if you use crashomon_store_symbols(). See INSTALL.md §Cross-compilation.
#
# When CRASHOMON_DUMP_SYMS_EXECUTABLE is not set, the breakpad_dump_syms target
# is not defined and install rules that reference it are skipped.
```

with:

```cmake
# ── breakpad_dump_syms ─────────────────────────────────────────────────────────
# CLI tool: extracts DWARF debug info from ELF binaries into Breakpad .sym files.
# Used by crashomon_store_symbols() as a POST_BUILD step.
#
# dump_syms is a host tool — it reads the binaries you built and must run on the
# build machine, not the target. Build it via cmake/host_toolkit/ and pass the
# result here via CRASHOMON_HOST_TOOLKIT_DIR (preferred) or directly:
#
#   cmake -B _host_toolkit -S cmake/host_toolkit/
#   cmake --build _host_toolkit --target host_toolkit
#   cmake -B build -DCRASHOMON_HOST_TOOLKIT_DIR=$(pwd)/_host_toolkit/bin
#
# For cross-compilation: build cmake/host_toolkit/ with the host compiler, then
# pass its bin/ directory as CRASHOMON_HOST_TOOLKIT_DIR in the cross build.
# See INSTALL.md §Cross-compilation.
#
# When CRASHOMON_DUMP_SYMS_EXECUTABLE is not set, the breakpad_dump_syms target
# is not defined and install rules that reference it are skipped.
```

- [ ] **Step 2: Update the FATAL_ERROR message in `cmake/crashomon_symbols.cmake`**

Replace lines 67–74:

```cmake
    message(FATAL_ERROR
      "crashomon_store_symbols(${target}): dump_syms host binary not configured.\n"
      "Build it once with the host compiler:\n"
      "  cmake -B _dump_syms_build -S <crashomon>/cmake/dump_syms_host/\n"
      "  cmake --build _dump_syms_build\n"
      "Then re-run configure with:\n"
      "  -DCRASHOMON_DUMP_SYMS_EXECUTABLE=<path>/_dump_syms_build/dump_syms")
```

with:

```cmake
    message(FATAL_ERROR
      "crashomon_store_symbols(${target}): dump_syms host binary not configured.\n"
      "Build the host toolkit:\n"
      "  cmake -B _host_toolkit -S <crashomon>/cmake/host_toolkit/\n"
      "  cmake --build _host_toolkit --target host_toolkit\n"
      "Then re-run configure with:\n"
      "  -DCRASHOMON_HOST_TOOLKIT_DIR=<path>/_host_toolkit/bin")
```

- [ ] **Step 3: Commit**

```bash
git add cmake/breakpad.cmake cmake/crashomon_symbols.cmake
git commit -m "docs(cmake): update dump_syms build instructions to use host_toolkit

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 4: Update `demo/CMakeLists.txt` and `demo/run_demo.sh`

**Files:**
- Modify: `demo/CMakeLists.txt`
- Modify: `demo/run_demo.sh`

- [ ] **Step 1: Remove `CRASHOMON_BUILD_ANALYZE` force-set from `demo/CMakeLists.txt`**

Remove lines 22–25 (the comment + force-set):

```cmake
# Force-enable minidump_stackwalk (required by crashomon-analyze).
set(CRASHOMON_BUILD_ANALYZE ON CACHE BOOL "Build minidump_stackwalk" FORCE)
```

- [ ] **Step 2: Fix `demo_paths.sh` generation in `demo/CMakeLists.txt`**

Replace the `file(GENERATE ...)` block:

```cmake
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/demo_paths.sh"
  CONTENT
"# Auto-generated by CMake — do not edit.
DEMO_CRASHER=\"$<TARGET_FILE:demo-crasher>\"
WATCHERD=\"$<TARGET_FILE:crashomon_watcherd>\"
MINIDUMP_STACKWALK=\"$<TARGET_FILE:minidump_stackwalk>\"
DUMP_SYMS=\"${CRASHOMON_DUMP_SYMS_EXECUTABLE}\"
LIBCRASHOMON_DIR=\"$<TARGET_FILE_DIR:crashomon_client>\"
"
)
```

with:

```cmake
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/demo_paths.sh"
  CONTENT
"# Auto-generated by CMake — do not edit.
DEMO_CRASHER=\"$<TARGET_FILE:demo-crasher>\"
WATCHERD=\"$<TARGET_FILE:crashomon_watcherd>\"
MINIDUMP_STACKWALK=\"${MINIDUMP_STACKWALK_EXECUTABLE}\"
DUMP_SYMS=\"${CRASHOMON_DUMP_SYMS_EXECUTABLE}\"
LIBCRASHOMON_DIR=\"$<TARGET_FILE_DIR:crashomon_client>\"
"
)
```

- [ ] **Step 3: Update `demo/run_demo.sh` to use `cmake/host_toolkit/` and new bin layout**

Replace the `HOST_BUILD` variable declaration (line 32):

```bash
HOST_BUILD="$DEMO_DIR/_host_build"     # standalone dump_syms build
```

with:

```bash
HOST_BUILD="$DEMO_DIR/_host_build"     # host_toolkit build
```

Replace the Phase 1a block (lines 84–99):

```bash
# ── 1a. Host tools: dump_syms and minidump_stackwalk ──────────────────────────
# dump_syms is a pre-built IMPORTED target in crashomon's CMake.  It must be
# compiled for the host (not the target) so that CI pipelines can run it on the
# build machine regardless of the target architecture.
if [[ -x "$HOST_BUILD/dump_syms" ]]; then
    _ok "Host tools already built (dump_syms, minidump_stackwalk)"
else
    echo "  Building host tools (dump_syms, minidump_stackwalk)..."
    cmake -S "$ROOT_DIR/cmake/dump_syms_host" -B "$HOST_BUILD" -Wno-dev \
        2>&1 | grep -v '^--' | tail -8 || true
    cmake --build "$HOST_BUILD" --parallel "$(nproc)"
    _ok "Host tools built: $HOST_BUILD/dump_syms"
fi

DUMP_SYMS_BIN="$HOST_BUILD/dump_syms"
[[ -x "$DUMP_SYMS_BIN" ]] || _die "dump_syms not found at $DUMP_SYMS_BIN"
```

with:

```bash
# ── 1a. Host tools: dump_syms, minidump-stackwalk, crashomon-analyze ───────────
# cmake/host_toolkit/ builds all three tools in one step.  Outputs land in
# _host_build/bin/.  Built with the host compiler; safe to reuse across
# cross-compilation builds.
if [[ -x "$HOST_BUILD/bin/dump_syms" ]]; then
    _ok "Host tools already built ($HOST_BUILD/bin/)"
else
    echo "  Building host tools..."
    cmake -S "$ROOT_DIR/cmake/host_toolkit" -B "$HOST_BUILD" -Wno-dev \
        2>&1 | grep -v '^--' | tail -8 || true
    cmake --build "$HOST_BUILD" --target host_toolkit --parallel "$(nproc)"
    _ok "Host tools built: $HOST_BUILD/bin/"
fi

DUMP_SYMS_BIN="$HOST_BUILD/bin/dump_syms"
[[ -x "$DUMP_SYMS_BIN" ]] || _die "dump_syms not found at $DUMP_SYMS_BIN"
```

Replace the demo cmake configure command (lines 106–108):

```bash
cmake -S "$DEMO_DIR" -B "$DEMO_BUILD" -Wno-dev \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$DUMP_SYMS_BIN" \
    2>&1 | grep -v '^--' | tail -8 || true
```

with:

```bash
cmake -S "$DEMO_DIR" -B "$DEMO_BUILD" -Wno-dev \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$HOST_BUILD/bin" \
    2>&1 | grep -v '^--' | tail -8 || true
```

- [ ] **Step 4: Commit**

```bash
git add demo/CMakeLists.txt demo/run_demo.sh
git commit -m "fix(demo): migrate from CRASHOMON_BUILD_ANALYZE to CRASHOMON_HOST_TOOLKIT_DIR

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 5: Update `test/demo_symbolicated.sh`

**Files:**
- Modify: `test/demo_symbolicated.sh:12-16,31-44`

- [ ] **Step 1: Update discovery comment and fallback paths**

Replace lines 12–16 (the comment block):

```bash
# dump_syms and minidump_stackwalk are auto-discovered from _dump_syms_build/ or
# co-located with minidump_stackwalk in BUILD_DIR. DUMP_SYMS_PATH can also be
# supplied via the CRASHOMON_DUMP_SYMS_EXECUTABLE environment variable.
# Build the host tools with:
#   cmake -B _dump_syms_build -S cmake/dump_syms_host/ && cmake --build _dump_syms_build
```

with:

```bash
# dump_syms and minidump-stackwalk are auto-discovered from _host_toolkit/bin/ or
# co-located in BUILD_DIR. DUMP_SYMS_PATH can also be supplied via the
# CRASHOMON_DUMP_SYMS_EXECUTABLE environment variable.
# Build the host tools with:
#   cmake -B _host_toolkit -S cmake/host_toolkit/ && cmake --build _host_toolkit --target host_toolkit
```

Replace the STACKWALK discovery block (lines 30–34):

```bash
# minidump_stackwalk: main build first, then host tools build.
STACKWALK="$(find "${BUILD_DIR}" -maxdepth 2 -name 'minidump_stackwalk' -type f 2>/dev/null | head -1 || true)"
if [[ -z "${STACKWALK}" ]]; then
  STACKWALK="$(find "${PROJECT_ROOT}/_dump_syms_build" -maxdepth 1 -name 'minidump_stackwalk' -type f 2>/dev/null | head -1 || true)"
fi
```

with:

```bash
# minidump-stackwalk: host_toolkit bin/ first, then BUILD_DIR fallback.
STACKWALK=""
if [[ -x "${PROJECT_ROOT}/_host_toolkit/bin/minidump-stackwalk" ]]; then
  STACKWALK="${PROJECT_ROOT}/_host_toolkit/bin/minidump-stackwalk"
fi
if [[ -z "${STACKWALK}" ]]; then
  STACKWALK="$(find "${BUILD_DIR}" -maxdepth 3 -name 'minidump-stackwalk' -type f 2>/dev/null | head -1 || true)"
fi
```

Replace the DUMP_SYMS discovery block (lines 36–45):

```bash
# dump_syms is a host binary built separately (see cmake/dump_syms_host/).
# Resolution order: CLI arg > env var > co-located with minidump_stackwalk > _dump_syms_build/.
DUMP_SYMS="${2:-${CRASHOMON_DUMP_SYMS_EXECUTABLE:-}}"
if [[ -z "${DUMP_SYMS}" ]] && [[ -n "${STACKWALK}" ]]; then
  CANDIDATE="$(dirname "${STACKWALK}")/dump_syms"
  [[ -f "${CANDIDATE}" ]] && DUMP_SYMS="${CANDIDATE}"
fi
if [[ -z "${DUMP_SYMS}" ]] && [[ -f "${PROJECT_ROOT}/_dump_syms_build/dump_syms" ]]; then
  DUMP_SYMS="${PROJECT_ROOT}/_dump_syms_build/dump_syms"
fi
```

with:

```bash
# dump_syms: CLI arg > env var > host_toolkit bin/ > co-located with stackwalk.
DUMP_SYMS="${2:-${CRASHOMON_DUMP_SYMS_EXECUTABLE:-}}"
if [[ -z "${DUMP_SYMS}" ]] && [[ -x "${PROJECT_ROOT}/_host_toolkit/bin/dump_syms" ]]; then
  DUMP_SYMS="${PROJECT_ROOT}/_host_toolkit/bin/dump_syms"
fi
if [[ -z "${DUMP_SYMS}" ]] && [[ -n "${STACKWALK}" ]]; then
  CANDIDATE="$(dirname "${STACKWALK}")/dump_syms"
  [[ -f "${CANDIDATE}" ]] && DUMP_SYMS="${CANDIDATE}"
fi
```

Replace the DUMP_SYMS error message (lines 54–57):

```bash
if [[ -z "${DUMP_SYMS}" ]]; then
  echo "ERROR: dump_syms not found. Build the host tools with:" >&2
  echo "  cmake -B _dump_syms_build -S cmake/dump_syms_host/ && cmake --build _dump_syms_build" >&2
  exit 1
fi
```

with:

```bash
if [[ -z "${DUMP_SYMS}" ]]; then
  echo "ERROR: dump_syms not found. Build the host tools with:" >&2
  echo "  cmake -B _host_toolkit -S cmake/host_toolkit/ && cmake --build _host_toolkit --target host_toolkit" >&2
  exit 1
fi
```

- [ ] **Step 2: Commit**

```bash
git add test/demo_symbolicated.sh
git commit -m "fix(test): update demo_symbolicated.sh discovery to use _host_toolkit/bin/

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 6: Update `DEVELOP.md` and `INSTALL.md`

**Files:**
- Modify: `DEVELOP.md`
- Modify: `INSTALL.md`

- [ ] **Step 1: Update `DEVELOP.md` cmake directory layout**

Replace lines 14–21 in the directory layout:

```
cmake/
  breakpad.cmake            Breakpad CMake targets (processor, stackwalk, dump_syms IMPORTED)
  crashomon_symbols.cmake   crashomon_store_symbols() function
  fetch_breakpad_deps.cmake FetchContent population for zlib, Breakpad, LSS (shared by main build
                            and dump_syms_host/)
  rust_minidump.cmake       ExternalProject build of minidump-stackwalk (statically linked via
                            +crt-static; exports MINIDUMP_STACKWALK_EXECUTABLE)
  store_sym_impl.cmake      POST_BUILD cmake -P script that runs dump_syms and writes the store
  dump_syms_host/           Standalone CMake project — builds the host dump_syms binary
```

with:

```
cmake/
  breakpad.cmake            Breakpad CMake targets (processor, dump_syms IMPORTED)
  crashomon_symbols.cmake   crashomon_store_symbols() function
  fetch_breakpad_deps.cmake FetchContent population for zlib, Breakpad, LSS (shared by main build
                            and host_toolkit/)
  store_sym_impl.cmake      POST_BUILD cmake -P script that runs dump_syms and writes the store
  host_toolkit/             Standalone CMake project — builds dump_syms, minidump-stackwalk,
                            and stages crashomon-analyze into <build>/bin/
```

- [ ] **Step 2: Update `DEVELOP.md` "Build first" section**

Replace lines 65–76:

```markdown
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
```

with:

```markdown
`ENABLE_TESTS=ON` adds the `examples/` targets, which call `crashomon_store_symbols()` and therefore require the host `dump_syms` binary. Build the host toolkit once before configuring:

```bash
# Build host tools once (dump_syms, minidump-stackwalk, crashomon-analyze)
cmake -B _host_toolkit -S cmake/host_toolkit/
cmake --build _host_toolkit --target host_toolkit -j$(nproc)

# Configure and build with tests
cmake -B build \
    -DENABLE_TESTS=ON \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin"
cmake --build build -j$(nproc)
```
```

- [ ] **Step 3: Update `DEVELOP.md` dump_syms smoke tests and sanitizer builds**

Replace line 124:

```bash
test/test_dump_syms_smoke.sh _dump_syms_build/dump_syms build/examples/crashomon-example-segfault
```

with:

```bash
test/test_dump_syms_smoke.sh _host_toolkit/bin/dump_syms build/examples/crashomon-example-segfault
```

Replace the sanitizer build commands in `DEVELOP.md`:

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

with:

```bash
cmake -B build-ubsan \
    -DENABLE_UBSAN=ON -DENABLE_TESTS=ON \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin"
cmake --build build-ubsan
ctest --test-dir build-ubsan --output-on-failure

cmake -B build-asan \
    -DENABLE_ASAN=ON -DENABLE_TESTS=ON \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin"
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

- [ ] **Step 4: Update `INSTALL.md` building section**

Replace the building section header through the end of the "Building `minidump-stackwalk`" subsection. Find the block starting with:

```markdown
`dump_syms` is a host tool that extracts DWARF debug info from ELF binaries into Breakpad `.sym` files. It must be built separately with the host compiler before the main build, whether you are cross-compiling or building natively:

```bash
# Build dump_syms for the host (once per machine)
cmake -B _dump_syms_build -S cmake/dump_syms_host/
cmake --build _dump_syms_build -j$(nproc)
# Produces: _dump_syms_build/dump_syms
```

Then configure and build the main project, passing the result in:

```bash
cmake -B build -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
cmake --build build -j$(nproc)

# Install Python deps (web UI + syms script)
uv sync
```
```

Replace with:

```markdown
`cmake/host_toolkit/` builds all three host tools in one step — `dump_syms`, `minidump-stackwalk`, and `crashomon-analyze`. Build it once before the main project:

```bash
# Build host tools (once per machine)
cmake -B _host_toolkit -S cmake/host_toolkit/
cmake --build _host_toolkit --target host_toolkit -j$(nproc)
# Produces: _host_toolkit/bin/{dump_syms,minidump-stackwalk,crashomon-analyze}
```

Then configure and build the main project:

```bash
cmake -B build -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin"
cmake --build build -j$(nproc)

# Install Python deps (web UI + syms script)
uv sync
```
```

Remove the now-redundant "Building `minidump-stackwalk` (required for symbolication)" subsection entirely (it was a separate build step; that's now handled by host_toolkit).

Update the "This produces:" listing to reflect the new output layout:

```markdown
This produces:

```
build/lib/libcrashomon.so       # LD_PRELOAD library
build/lib/libcrashomon.a        # static library
build/daemon/crashomon-watcherd # watcher daemon (also hosts crash handler)
_host_toolkit/bin/dump_syms             # DWARF → .sym extraction tool (host binary)
_host_toolkit/bin/minidump-stackwalk    # symbolication engine (host binary)
_host_toolkit/bin/crashomon-analyze     # CLI symbolication (staged Python script)
tools/syms/crashomon-syms       # symbol store management (Python script, no build step)
```
```

- [ ] **Step 5: Update INSTALL.md cross-compilation section**

Find and replace Step 1 in the cross-compilation section:

```markdown
### Step 1: Build `dump_syms` for the host

`dump_syms` is the Breakpad tool that extracts DWARF debug info from ELF binaries into `.sym` files...

```bash
cmake -B _dump_syms_build -S cmake/dump_syms_host/
cmake --build _dump_syms_build -j$(nproc)
# Produces: _dump_syms_build/dump_syms
```
```

with:

```markdown
### Step 1: Build the host toolkit

The host toolkit (`dump_syms`, `minidump-stackwalk`, `crashomon-analyze`) runs on the
**build machine**, not the target device. Building it with a cross-compiler produces
target-architecture binaries that cannot execute on the host, so it must be built
separately with the host compiler before the cross-compilation step.

```bash
cmake -B _host_toolkit -S cmake/host_toolkit/
cmake --build _host_toolkit --target host_toolkit -j$(nproc)
# Produces: _host_toolkit/bin/{dump_syms,minidump-stackwalk,crashomon-analyze}
```

This only needs to be done once per host machine. `dump_syms` links zlib and libstdc++
statically and has no runtime dependencies beyond libc.
```

Find and replace Step 3's cmake configure command:

```bash
cmake -B build-cross \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms"
```

with:

```bash
cmake -B build-cross \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin"
```

Also update Step 4 Option B's FetchContent example:

```bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" \
    -DCRASOMON_SYMBOL_STORE=/srv/crashomon/symbols
```

with:

```bash
cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain-aarch64.cmake \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin" \
    -DCRASOMON_SYMBOL_STORE=/srv/crashomon/symbols
```

Also update the optional build flags section:

```bash
cmake -B build -DENABLE_TESTS=ON -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" && cmake --build build
```

with:

```bash
cmake -B build -DENABLE_TESTS=ON -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin" && cmake --build build
```

- [ ] **Step 6: Commit**

```bash
git add DEVELOP.md INSTALL.md
git commit -m "docs: update build instructions to use cmake/host_toolkit/

Replace all references to cmake/dump_syms_host/, _dump_syms_build/,
CRASHOMON_BUILD_ANALYZE, and CRASHOMON_DUMP_SYMS_EXECUTABLE (as a manual
flag) with the new host_toolkit workflow.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 7: Remove old files

**Files:**
- Delete: `cmake/dump_syms_host/CMakeLists.txt`
- Delete: `cmake/rust_minidump.cmake`

- [ ] **Step 1: Remove superseded files**

```bash
git rm cmake/dump_syms_host/CMakeLists.txt cmake/rust_minidump.cmake
```

- [ ] **Step 2: Commit**

```bash
git commit -m "chore(cmake): remove cmake/dump_syms_host/ and cmake/rust_minidump.cmake

Both superseded by cmake/host_toolkit/CMakeLists.txt.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```

---

## Task 8: End-to-end verification

- [ ] **Step 1: Build host_toolkit fully (dump_syms + stackwalk + analyze)**

```bash
cmake --build _host_toolkit --target host_toolkit -j$(nproc) 2>&1 | tail -10
```

Expected: all three in `_host_toolkit/bin/`:
```
_host_toolkit/bin/dump_syms
_host_toolkit/bin/minidump-stackwalk
_host_toolkit/bin/crashomon-analyze
```

Verify:
```bash
ls -la _host_toolkit/bin/
```

- [ ] **Step 2: Configure and build main project with CRASHOMON_HOST_TOOLKIT_DIR**

```bash
cmake -B build \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin" \
    -Wno-dev 2>&1 | tail -5
cmake --build build -j$(nproc) 2>&1 | tail -5
```

Expected: configure and build succeed

- [ ] **Step 3: Build with ENABLE_TESTS**

```bash
cmake -B build \
    -DENABLE_TESTS=ON \
    -DCRASHOMON_HOST_TOOLKIT_DIR="$(pwd)/_host_toolkit/bin" \
    -Wno-dev
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Expected: all tests pass

- [ ] **Step 4: Verify CRASHOMON_INSTALL_ANALYZE FATAL_ERROR fires correctly**

```bash
cmake -B build-check -DCRASHOMON_INSTALL_ANALYZE=ON -Wno-dev 2>&1 | grep -A5 "FATAL_ERROR\|CRASHOMON_INSTALL_ANALYZE"
rm -rf build-check
```

Expected: configure fails with the FATAL_ERROR message about `MINIDUMP_STACKWALK_EXECUTABLE`

- [ ] **Step 5: Verify dump_syms smoke test still works**

```bash
test/test_dump_syms_smoke.sh _host_toolkit/bin/dump_syms build/examples/crashomon-example-segfault
```

Expected: `PASS` for all checks

- [ ] **Step 6: Final commit if anything was missed**

If no changes needed, skip. Otherwise:

```bash
git add -p
git commit -m "fix: address verification issues

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```
