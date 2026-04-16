cmake_minimum_required(VERSION 3.25)
project(dump_syms_host C CXX)

# Standalone build for the Breakpad dump_syms host tool.
# Build this once on a machine with a host C++ compiler, then ship the resulting
# binary in the SDK toolchain and point CRASHOMON_DUMP_SYMS_EXECUTABLE at it.
#
# Usage:
#   cmake -B build -S cmake/dump_syms_host/
#   cmake --build build
#   cmake --install build --prefix /path/to/toolchain
#
# Linking: zlib/libstdc++/libgcc linked statically; libc dynamic.

# ── Language standards ─────────────────────────────────────────────────────────

set(CMAKE_C_STANDARD   11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD   17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Fetch Breakpad, zlib, LSS ─────────────────────────────────────────────────

include("${CMAKE_CURRENT_LIST_DIR}/../fetch_breakpad_deps.cmake")

set(BREAKPAD_SRC "${crashomon_breakpad_SOURCE_DIR}/src")

# ── zlib (static) ─────────────────────────────────────────────────────────────

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${crashomon_zlib_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/zlib-build" EXCLUDE_FROM_ALL)
target_compile_options(zlibstatic PRIVATE -w)
set_target_properties(zlibstatic PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/zlib-build"
)

# ── dump_syms executable ───────────────────────────────────────────────────────
# Sources are explicit (no glob) to avoid pulling in test/platform-specific files.

add_executable(dump_syms
  # DWARF parsing
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
  # Linux ELF utilities
  "${BREAKPAD_SRC}/common/linux/crc32.cc"
  "${BREAKPAD_SRC}/common/linux/dump_symbols.cc"
  "${BREAKPAD_SRC}/common/linux/elf_symbols_to_module.cc"
  "${BREAKPAD_SRC}/common/linux/elfutils.cc"
  "${BREAKPAD_SRC}/common/linux/file_id.cc"
  "${BREAKPAD_SRC}/common/linux/linux_libc_support.cc"
  "${BREAKPAD_SRC}/common/linux/memory_mapped_file.cc"
  "${BREAKPAD_SRC}/common/linux/safe_readlink.cc"
  # dump_syms main
  "${BREAKPAD_SRC}/tools/linux/dump_syms/dump_syms.cc"
)

target_compile_options(dump_syms PRIVATE
  -w          # No warnings on Breakpad code (third-party)
  -DN_UNDF=0  # N_UNDF (stabs type 0) absent from glibc's stab.h; define it
)
target_include_directories(dump_syms PRIVATE "${BREAKPAD_SRC}")
target_link_libraries(dump_syms PRIVATE zlibstatic)
target_link_options(dump_syms PRIVATE -static-libstdc++ -static-libgcc)

# ── Install ────────────────────────────────────────────────────────────────────

include(GNUInstallDirs)
install(TARGETS dump_syms RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
