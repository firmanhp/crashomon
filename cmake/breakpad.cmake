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
#   minidump_stackwalk  — symbolication CLI tool (EXECUTABLE)
#   breakpad_dump_syms  — DWARF → .sym extraction tool (EXECUTABLE)
#
# Consumers link breakpad_processor into their own targets. The tool executables
# are built as part of the project (installed alongside other binaries).

# ── Shared include directories ─────────────────────────────────────────────────

set(BREAKPAD_SRC "${breakpad_SOURCE_DIR}/src")

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

# ── minidump_stackwalk ─────────────────────────────────────────────────────────
# CLI tool: symbolicates a minidump against a Breakpad symbol store.
# Used by crashomon-analyze and crashomon-web as a subprocess.
# Skipped when neither the analyze install component nor the explicit build
# toggle is requested — avoids compiling Breakpad's stackwalk sources in
# subproject / embedder configurations that only need the client + watcherd.

if(CRASHOMON_INSTALL_ANALYZE OR CRASHOMON_BUILD_ANALYZE)
  add_executable(minidump_stackwalk
    "${BREAKPAD_SRC}/processor/minidump_stackwalk.cc"
  )
  target_compile_options(minidump_stackwalk PRIVATE -w)
  target_include_directories(minidump_stackwalk PRIVATE "${BREAKPAD_SRC}")
  target_link_libraries(minidump_stackwalk PRIVATE breakpad_processor)
endif()

# ── breakpad_dump_syms ─────────────────────────────────────────────────────────
# CLI tool: extracts DWARF debug info from ELF binaries into Breakpad .sym files.
# Used by crashomon-syms and crashomon-web as a subprocess.
#
# Sources are explicit (no glob) to avoid pulling in test/platform-specific files.

add_executable(breakpad_dump_syms
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
target_compile_options(breakpad_dump_syms PRIVATE
  -w
  # N_UNDF (stabs symbol type 0) is not in glibc's stab.h enum on Linux.
  # Define it explicitly for portability.
  -DN_UNDF=0
)
target_include_directories(breakpad_dump_syms PRIVATE "${BREAKPAD_SRC}")
target_link_libraries(breakpad_dump_syms
  PRIVATE
    Threads::Threads
    ZLIB::ZLIB
)

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
# ${zlib_dep_BINARY_DIR}/libz.a. CMake does not automatically infer a build
# dependency from an IMPORTED_LOCATION path, so without an explicit
# add_dependencies the linker invocation for each target can run before
# zlibstatic has been compiled, producing "No rule to make target libz.a".
foreach(_bp_tgt IN ITEMS breakpad_processor minidump_stackwalk breakpad_dump_syms)
  if(TARGET ${_bp_tgt} AND TARGET zlibstatic)
    add_dependencies(${_bp_tgt} zlibstatic)
  endif()
endforeach()

# ── Thread support (required by Threads::Threads) ──────────────────────────────
find_package(Threads REQUIRED)
