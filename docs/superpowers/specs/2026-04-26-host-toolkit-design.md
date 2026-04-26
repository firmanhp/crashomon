# Host Toolkit Consolidation — Design Spec

**Date:** 2026-04-26
**Status:** Approved

## Problem

Building and locating the host-side developer tools currently requires three scattered manual steps:

1. Build `dump_syms` via a standalone sub-project (`cmake/dump_syms_host/`) and pass the result as `CRASHOMON_DUMP_SYMS_EXECUTABLE`.
2. Build `minidump-stackwalk` by enabling `-DCRASHOMON_BUILD_ANALYZE=ON` in the main build (a separate opt-in that triggers `cmake/rust_minidump.cmake`).
3. Locate `crashomon-analyze` in the source tree manually.

These are logically a single unit — tools that run on the developer's host machine for crash analysis and symbol extraction. There is no consolidated entry point.

## Goal

Introduce a `host_toolkit` CMake target that builds `dump_syms`, `minidump-stackwalk`, and stages `crashomon-analyze` in one shot. The developer retains full control over which compiler and toolchain builds these tools.

## Design

### New standalone project: `cmake/host_toolkit/`

`cmake/host_toolkit/CMakeLists.txt` is a self-contained CMake project. It replaces and supersedes both `cmake/dump_syms_host/` and the `cmake/rust_minidump.cmake` integration in the main build.

It contains three build units:

**`dump_syms`** — compiled as a regular CMake executable using the same sources as the current `cmake/dump_syms_host/`. Inherits whichever compiler the developer configured. `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is set to `${CMAKE_BINARY_DIR}/bin` so the binary lands there automatically.

**`minidump-stackwalk`** — built via `ExternalProject_Add` using Cargo (unchanged from current `cmake/rust_minidump.cmake`). Must remain ExternalProject because Cargo is not CMake. The `host_toolkit` target copies the resulting binary into `${CMAKE_BINARY_DIR}/bin/`.

**`crashomon-analyze` staging** — `add_custom_command` copies `tools/analyze/crashomon-analyze` from the source tree into `${CMAKE_BINARY_DIR}/bin/`. The source path is resolved via `CMAKE_CURRENT_SOURCE_DIR` (`cmake/host_toolkit/../../tools/analyze/crashomon-analyze`), which is always valid since the project is built in-tree.

**`host_toolkit` aggregator target** — depends on all three units. After `cmake --build _host_toolkit --target host_toolkit`, the output is:

```
_host_toolkit/bin/
  dump_syms
  minidump-stackwalk
  crashomon-analyze
```

### Downstream consumer usage (Case A)

A developer with their own host-only CMake project can pull `host_toolkit` into their build graph directly:

```cmake
add_subdirectory(path/to/crashomon/cmake/host_toolkit)
add_dependencies(my_host_step host_toolkit)
```

The target participates in their build with their compiler. No extra configuration needed.

### Main `CMakeLists.txt` changes

**Added:** `CRASHOMON_HOST_TOOLKIT_DIR` cache variable (PATH). When set, auto-derives `CRASHOMON_DUMP_SYMS_EXECUTABLE` and `MINIDUMP_STACKWALK_EXECUTABLE`:

```cmake
set(CRASHOMON_HOST_TOOLKIT_DIR "" CACHE PATH
  "Directory containing host_toolkit outputs (dump_syms, minidump-stackwalk, crashomon-analyze)")

if(CRASHOMON_HOST_TOOLKIT_DIR)
  if(NOT CRASHOMON_DUMP_SYMS_EXECUTABLE)
    set(CRASHOMON_DUMP_SYMS_EXECUTABLE
      "${CRASHOMON_HOST_TOOLKIT_DIR}/dump_syms"
      CACHE FILEPATH "" FORCE)
  endif()
  if(NOT MINIDUMP_STACKWALK_EXECUTABLE)
    set(MINIDUMP_STACKWALK_EXECUTABLE
      "${CRASHOMON_HOST_TOOLKIT_DIR}/minidump-stackwalk"
      CACHE FILEPATH "" FORCE)
  endif()
endif()
```

This block runs before `cmake/breakpad.cmake` is included, so derived values are visible to all downstream consumers.

**Removed:**
- `CRASHOMON_BUILD_ANALYZE` option
- `if(CRASHOMON_INSTALL_ANALYZE OR CRASHOMON_BUILD_ANALYZE) include(cmake/rust_minidump.cmake) endif()` block

**Kept (backwards compat):**
- `CRASHOMON_DUMP_SYMS_EXECUTABLE` — direct override still works; `CRASHOMON_HOST_TOOLKIT_DIR` only sets it when not already set
- `CRASHOMON_INSTALL_ANALYZE` — install rules unchanged; now requires `CRASHOMON_HOST_TOOLKIT_DIR` (or direct `MINIDUMP_STACKWALK_EXECUTABLE`) instead of `CRASHOMON_BUILD_ANALYZE`
- All existing install components

**Removed from repo:**
- `cmake/dump_syms_host/` — superseded by `cmake/host_toolkit/`
- `cmake/rust_minidump.cmake` — logic moves into `cmake/host_toolkit/`

### Developer workflow (after)

```bash
# Build host tools once — full compiler/toolchain control
cmake -B _host_toolkit -S cmake/host_toolkit/   # any cmake flags apply here
cmake --build _host_toolkit --target host_toolkit

# Main project — no scattered flags
cmake -B build -DCRASHOMON_HOST_TOOLKIT_DIR=$(pwd)/_host_toolkit/bin
cmake --build build
```

**Cross-compilation** is unchanged in shape — build host_toolkit with the host compiler, main build with the cross-compiler toolchain, pass `CRASHOMON_HOST_TOOLKIT_DIR` across:

```bash
cmake -B _host_toolkit -S cmake/host_toolkit/ -DCMAKE_CXX_COMPILER=x86_64-linux-gnu-g++
cmake --build _host_toolkit --target host_toolkit

cmake -B build-cross -DCMAKE_TOOLCHAIN_FILE=toolchain-aarch64.cmake \
    -DCRASHOMON_HOST_TOOLKIT_DIR=$(pwd)/_host_toolkit/bin
cmake --build build-cross
```

## Files changed

| File | Change |
|---|---|
| `cmake/host_toolkit/CMakeLists.txt` | New — standalone host tools project |
| `CMakeLists.txt` | Add `CRASHOMON_HOST_TOOLKIT_DIR`; remove `CRASHOMON_BUILD_ANALYZE` and `rust_minidump.cmake` include |
| `cmake/dump_syms_host/` | Removed — superseded |
| `cmake/rust_minidump.cmake` | Removed — logic moves to `cmake/host_toolkit/` |
| `INSTALL.md` | Update build instructions to new workflow |
