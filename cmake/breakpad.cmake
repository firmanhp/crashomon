# cmake/breakpad.cmake — CMake targets for Google Breakpad
#
# Breakpad has no native CMake build (it uses GN/autotools). This file defines
# CMake targets from the FetchContent-populated source tree, following the same
# approach used by vcpkg. All targets suppress compiler warnings (-w) since we
# do not apply -Werror to third-party code.
#
# Targets defined:
#   breakpad_processor  — minidump parsing library (STATIC); used by watcherd
#   breakpad_dump_syms  — DWARF → .sym extraction tool (EXECUTABLE)
#
# Consumers link breakpad_processor into their own targets. The tool executables
# are built as part of the project (installed alongside other binaries).

# ── Shared include directories ─────────────────────────────────────────────────

set(BREAKPAD_SRC "${crashomon_breakpad_SOURCE_DIR}/src")

# ── breakpad_processor ─────────────────────────────────────────────────────────
# Minimal minidump reader. Provides google_breakpad::Minidump and its stream
# accessors (exception info, thread list, module list, register context).
# Only the files transitively required by minidump_reader.cpp are listed —
# stackwalkers, symbolication, disassembler, and microdump code are excluded.

add_library(breakpad_processor STATIC
  "${BREAKPAD_SRC}/processor/minidump.cc"
  "${BREAKPAD_SRC}/processor/logging.cc"
  "${BREAKPAD_SRC}/processor/basic_code_modules.cc"
  "${BREAKPAD_SRC}/processor/convert_old_arm64_context.cc"
  "${BREAKPAD_SRC}/processor/dump_context.cc"
  "${BREAKPAD_SRC}/processor/dump_object.cc"
  "${BREAKPAD_SRC}/processor/pathname_stripper.cc"
  "${BREAKPAD_SRC}/processor/proc_maps_linux.cc"
  "${BREAKPAD_SRC}/common/linux/scoped_pipe.cc"
  "${BREAKPAD_SRC}/common/linux/scoped_tmpfile.cc"
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
target_link_libraries(breakpad_processor PRIVATE Threads::Threads)

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

add_library(breakpad_synth_minidump STATIC
  "${BREAKPAD_SRC}/processor/synth_minidump.cc"
  "${BREAKPAD_SRC}/common/test_assembler.cc"
)
target_compile_options(breakpad_synth_minidump PRIVATE -w)
target_include_directories(breakpad_synth_minidump PUBLIC "${BREAKPAD_SRC}")

# ── Thread support (required by Threads::Threads) ──────────────────────────────
find_package(Threads REQUIRED)
