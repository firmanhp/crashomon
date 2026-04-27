cmake_minimum_required(VERSION 3.25)
project(crashomon_host_toolkit C CXX)

# ── Language standards ─────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Portability: ISA baseline ──────────────────────────────────────────────────
# Pin to x86-64 baseline (SSE2 only) so precompiled binaries run on any x86-64
# machine regardless of what the build host supports. Without this, a build on
# a Haswell+ host can silently emit AVX2 from auto-vectorization.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_compile_options(-march=x86-64 -mtune=generic)
endif()

# All executables land in bin/ so CRASHOMON_HOST_TOOLKIT_DIR can point at a
# single flat directory containing dump_syms, minidump-stackwalk,
# crashomon-analyze, and crashomon-syms.
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

set(_RUST_EXTRA_FLAGS "")
if(_RUST_HOST_TRIPLE MATCHES "^x86_64")
  string(APPEND _RUST_EXTRA_FLAGS "-C target-cpu=x86-64")
endif()

ExternalProject_Add(rust_minidump_build
  GIT_REPOSITORY https://github.com/rust-minidump/rust-minidump.git
  GIT_TAG        v0.26.1
  GIT_SHALLOW    TRUE
  CONFIGURE_COMMAND ""
  BUILD_COMMAND
    ${CMAKE_COMMAND} -E env
      "${_RUSTFLAGS_VAR}=-C target-feature=+crt-static ${_RUST_EXTRA_FLAGS}"
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
unset(_RUST_EXTRA_FLAGS)

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

# ── crashomon-syms staging ────────────────────────────────────────────────────
set(_SYMS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/syms/crashomon-syms")
set(_SYMS_OUT "${CMAKE_BINARY_DIR}/bin/crashomon-syms")

add_custom_command(
  OUTPUT  "${_SYMS_OUT}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy "${_SYMS_SRC}" "${_SYMS_OUT}"
  DEPENDS "${_SYMS_SRC}"
  VERBATIM
)
add_custom_target(host_toolkit_stage_syms DEPENDS "${_SYMS_OUT}")

# ── crashomon-patchdmp staging ────────────────────────────────────────────────
set(_PATCHDMP_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/patchdmp/crashomon-patchdmp")
set(_PATCHDMP_OUT "${CMAKE_BINARY_DIR}/bin/crashomon-patchdmp")

add_custom_command(
  OUTPUT  "${_PATCHDMP_OUT}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin"
  COMMAND ${CMAKE_COMMAND} -E copy "${_PATCHDMP_SRC}" "${_PATCHDMP_OUT}"
  DEPENDS "${_PATCHDMP_SRC}"
  VERBATIM
)
add_custom_target(host_toolkit_stage_patchdmp DEPENDS "${_PATCHDMP_OUT}")

# ── crashomon_tools Python package staging ────────────────────────────────────
# Staged alongside the scripts so Python's script-directory resolution finds it
# without requiring a separate pip install step.
set(_PYLIB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/crashomon_tools")
set(_PYLIB_OUT "${CMAKE_BINARY_DIR}/bin/crashomon_tools")

add_custom_command(
  OUTPUT  "${_PYLIB_OUT}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${_PYLIB_SRC}" "${_PYLIB_OUT}"
  DEPENDS "${_PYLIB_SRC}"
  VERBATIM
)
add_custom_target(host_toolkit_stage_pylib DEPENDS "${_PYLIB_OUT}")

# ── host_toolkit aggregator ───────────────────────────────────────────────────
add_custom_target(host_toolkit
  DEPENDS dump_syms host_toolkit_stage_stackwalk host_toolkit_stage_analyze
          host_toolkit_stage_syms host_toolkit_stage_patchdmp host_toolkit_stage_pylib
)

# ── Install ────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS dump_syms RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${_STACKWALK_SRC}" DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${_ANALYZE_SRC}"   DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${_SYMS_SRC}"      DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS "${_PATCHDMP_SRC}"  DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${_PYLIB_SRC}"    DESTINATION ${CMAKE_INSTALL_BINDIR})
