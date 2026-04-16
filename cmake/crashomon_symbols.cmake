# cmake/crashomon_symbols.cmake — CMake symbol store integration for crashomon.
#
# Provides:
#   crashomon_store_symbols(<target> [STORE <dir>] [WITH_BINARY])
#
# Attaches a POST_BUILD step to <target> that runs breakpad_dump_syms on the
# freshly linked binary and stores the resulting .sym file in the Breakpad
# symbol store layout:
#
#   <store>/<module_name>/<build_id>/<module_name>.sym
#
# The layout is content-addressed by build ID: symbols from different builds
# never collide, so CI/CD pipelines can rsync multiple build outputs into a
# single shared store.
#
# The produced store is consumed directly by crashomon-analyze:
#
#   # Minidump analysis (primary use case):
#   crashomon-analyze --store <dir> --minidump crash.dmp
#
#   # Tombstone text from journalctl (requires WITH_BINARY):
#   crashomon-analyze --debug-dir <dir> --stdin < tombstone.txt
#
# Options:
#   STORE <dir>   — override CRASHOMON_SYMBOL_STORE for this target only
#   WITH_BINARY   — also copy the unstripped ELF into the store entry, enabling
#                   --debug-dir --stdin mode in crashomon-analyze
#
# Cache variables:
#   CRASHOMON_SYMBOL_STORE  — store root, default ${CMAKE_BINARY_DIR}/symbols
#                             Override at configure time:
#                               cmake -B build -DCRASOMON_SYMBOL_STORE=/srv/symbols
#
# Requires CRASHOMON_DUMP_SYMS_EXECUTABLE to be set (see cmake/breakpad.cmake).
# If the breakpad_dump_syms target is absent, configure fails with FATAL_ERROR.

# ── Cache variable ─────────────────────────────────────────────────────────────
# Declared here (not in the root CMakeLists.txt) so it is available in both
# top-level and FetchContent/add_subdirectory configurations.

set(CRASHOMON_SYMBOL_STORE "${CMAKE_BINARY_DIR}/symbols" CACHE PATH
  "Root directory for the Breakpad symbol store populated by crashomon_store_symbols()")

# ── Path to the build-time helper script ──────────────────────────────────────
# CMAKE_CURRENT_LIST_DIR resolves to the cmake/ directory at include time,
# regardless of which CMakeLists.txt included this file. CACHE INTERNAL hides
# the variable from cmake-gui without affecting consumers.

set(_CRASHOMON_STORE_SYM_IMPL
  "${CMAKE_CURRENT_LIST_DIR}/store_sym_impl.cmake"
  CACHE INTERNAL "")

# ── Function definition ────────────────────────────────────────────────────────

function(crashomon_store_symbols target)
  # PARSE_ARGV 1 skips the positional 'target' argument during keyword parsing.
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
    message(FATAL_ERROR
      "crashomon_store_symbols(${target}): dump_syms host binary not configured.\n"
      "Build it once with the host compiler:\n"
      "  cmake -B _dump_syms_build -S <crashomon>/cmake/dump_syms_host/\n"
      "  cmake --build _dump_syms_build\n"
      "Then re-run configure with:\n"
      "  -DCRASHOMON_DUMP_SYMS_EXECUTABLE=<path>/_dump_syms_build/dump_syms")
  endif()

  # Generator expressions ($<TARGET_FILE:...>) are evaluated at build time by
  # the Makefile/Ninja backend and injected as the -D values seen by the cmake
  # -P script. This is the standard pattern for POST_BUILD tool invocations.
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
