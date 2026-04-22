# cmake/breakpad.cmake — CMake targets for Google Breakpad
#
# Breakpad has no native CMake build (it uses GN/autotools). This file defines
# CMake targets from the FetchContent-populated source tree, following the same
# approach used by vcpkg. All targets suppress compiler warnings (-w) since we
# do not apply -Werror to third-party code.
#
# Targets defined:
#   breakpad_libdisasm  — vendored x86 disassembler (STATIC)
#   breakpad_processor  — minidump parsing library (STATIC); used by watcherd
#   breakpad_dump_syms  — DWARF → .sym extraction tool (EXECUTABLE)
#
# Consumers link breakpad_processor into their own targets. The tool executables
# are built as part of the project (installed alongside other binaries).

# ── Shared include directories ─────────────────────────────────────────────────

set(BREAKPAD_SRC "${crashomon_breakpad_SOURCE_DIR}/src")

# ── breakpad_libdisasm ─────────────────────────────────────────────────────────
# Vendored x86 disassembler used by the stack scanner in the processor library.

file(GLOB _libdisasm_sources "${BREAKPAD_SRC}/third_party/libdisasm/*.c")

add_library(breakpad_libdisasm STATIC ${_libdisasm_sources})
target_compile_options(breakpad_libdisasm PRIVATE -w)
target_include_directories(breakpad_libdisasm PUBLIC "${BREAKPAD_SRC}/third_party/libdisasm")

# ── breakpad_processor ─────────────────────────────────────────────────────────
# Minidump processing library. Parses minidump files, extracts thread stacks,
# module lists, and register state. Used by crashomon-watcherd.

file(GLOB _processor_sources "${BREAKPAD_SRC}/processor/*.cc")

# Exclude: unit/self tests, synthesis helpers, standalone tool mains,
#          and platform-specific directories not built on Linux.
list(FILTER _processor_sources EXCLUDE REGEX
  "(_unittest|_selftest|synth_minidump|_test)\\.cc$|minidump_stackwalk\\.cc$|minidump_dump\\.cc$|microdump_stackwalk\\.cc$"
)

add_library(breakpad_processor STATIC
  ${_processor_sources}
  # common/linux helpers required by processor internals (disassembler_objdump, etc.)
  "${BREAKPAD_SRC}/common/linux/scoped_pipe.cc"
  "${BREAKPAD_SRC}/common/linux/scoped_tmpfile.cc"
  # path_helper provides BaseName() used by minidump_stackwalk
  "${BREAKPAD_SRC}/common/path_helper.cc"
)
target_compile_options(breakpad_processor PRIVATE -w)
# Suppress INFO-level BPLOG() calls. Each BPLOG(INFO) constructs a LogStream
# which calls localtime_r() — glibc's tzset lock is not held when the return
# value (tzname[0]) is read back by Crashpad's TimeZone(), causing a race that
# produces a NULL tzname[0] and a strlen(NULL) SIGSEGV under concurrent load.
# Raising the minimum severity to ERROR eliminates the concurrent localtime_r()
# calls from the main thread, removing the race.
target_compile_definitions(breakpad_processor PRIVATE
  BPLOG_MINIMUM_SEVERITY=SEVERITY_ERROR
)
target_include_directories(breakpad_processor PUBLIC "${BREAKPAD_SRC}")
target_link_libraries(breakpad_processor
  PRIVATE
    breakpad_libdisasm
    Threads::Threads
    ZLIB::ZLIB
)

# ── breakpad_dump_syms ─────────────────────────────────────────────────────────
# CLI tool: extracts DWARF debug info from ELF binaries into Breakpad .sym files.
# Used by crashomon_store_symbols() as a POST_BUILD step.
#
# dump_syms must be a pre-built host binary — set CRASHOMON_DUMP_SYMS_EXECUTABLE
# to its path.  See cmake/dump_syms_host/ for the build recipe.

set(CRASHOMON_DUMP_SYMS_EXECUTABLE "" CACHE FILEPATH
  "Path to the pre-built host dump_syms binary.")

if(CRASHOMON_DUMP_SYMS_EXECUTABLE)
  if(NOT EXISTS "${CRASHOMON_DUMP_SYMS_EXECUTABLE}")
    message(FATAL_ERROR
      "CRASHOMON_DUMP_SYMS_EXECUTABLE='${CRASHOMON_DUMP_SYMS_EXECUTABLE}' does not exist.")
  endif()
  add_executable(breakpad_dump_syms IMPORTED GLOBAL)
  set_target_properties(breakpad_dump_syms PROPERTIES
    IMPORTED_LOCATION "${CRASHOMON_DUMP_SYMS_EXECUTABLE}"
  )
endif()

# ── breakpad_synth_minidump ────────────────────────────────────────────────────
# SynthMinidump + test_assembler: used by gen_synthetic_fixtures (test-only).
# Not compiled into breakpad_processor (excluded by its filter regex above).

add_library(breakpad_synth_minidump STATIC
  "${BREAKPAD_SRC}/processor/synth_minidump.cc"
  "${BREAKPAD_SRC}/common/test_assembler.cc"
)
target_compile_options(breakpad_synth_minidump PRIVATE -w)
target_include_directories(breakpad_synth_minidump PUBLIC "${BREAKPAD_SRC}")

# ── Build ordering: zlibstatic must exist before Breakpad targets link ─────────
# ZLIB::ZLIB is an IMPORTED target whose IMPORTED_LOCATION points to
# ${crashomon_zlib_BINARY_DIR}/libz.a. CMake does not automatically infer a build
# dependency from an IMPORTED_LOCATION path, so without an explicit
# add_dependencies the linker invocation for each target can run before
# zlibstatic has been compiled, producing "No rule to make target libz.a".
#
# breakpad_dump_syms is excluded: it is a pre-built IMPORTED binary and has
# no link step against the outer build's zlibstatic.
if(TARGET breakpad_processor AND TARGET zlibstatic)
  add_dependencies(breakpad_processor zlibstatic)
endif()

# ── Thread support (required by Threads::Threads) ──────────────────────────────
find_package(Threads REQUIRED)
