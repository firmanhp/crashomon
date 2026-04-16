# cmake/crashomon_symbols.cmake — CMake symbol store integration for crashomon.
#
# Provides:
#   crashomon_store_symbols(<target> [STORE <dir>] [WITH_BINARY])
#       Attaches a POST_BUILD step to <target> that stores its .sym file.
#
#   crashomon_store_sysroot_symbols([SYSROOT <dir>] [STORE <dir>] [SEARCH_PATHS <p..>])
#       Creates 'crashomon_sysroot_symbols' target; scans a cross-compilation
#       sysroot for shared libraries and populates the same symbol store.
#       Run explicitly: cmake --build <dir> --target crashomon_sysroot_symbols
#
# crashomon_store_symbols() attaches a POST_BUILD step to <target> that runs
# breakpad_dump_syms on the freshly linked binary and stores the resulting
# .sym file in the Breakpad symbol store layout:
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

# ── Sysroot symbol extraction ────────────────────────────────────────────────
# Path to the sysroot helper script, resolved once at include time.

set(_CRASHOMON_STORE_SYSROOT_IMPL
  "${CMAKE_CURRENT_LIST_DIR}/store_sysroot_impl.cmake"
  CACHE INTERNAL "")

# crashomon_store_sysroot_symbols([SYSROOT <dir>] [STORE <dir>] [SEARCH_PATHS <paths...>])
#
# Creates a custom target 'crashomon_sysroot_symbols' that scans a cross-
# compilation sysroot for shared libraries, extracts Breakpad symbols via
# dump_syms, and stores them in the symbol store.
#
# The target is NOT added to ALL — run it explicitly:
#   cmake --build <build-dir> --target crashomon_sysroot_symbols
#
# Or make it a dependency of your app target:
#   add_dependencies(myapp crashomon_sysroot_symbols)
#
# Options:
#   SYSROOT <dir>       — sysroot root (default: CMAKE_SYSROOT)
#   STORE <dir>         — symbol store root (default: CRASHOMON_SYMBOL_STORE)
#   SEARCH_PATHS <p..>  — relative paths under SYSROOT to scan
#                         (default: usr/lib)

function(crashomon_store_sysroot_symbols)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "" "SYSROOT;STORE" "SEARCH_PATHS")

  if(ARG_SYSROOT)
    set(_sysroot "${ARG_SYSROOT}")
  elseif(CMAKE_SYSROOT)
    set(_sysroot "${CMAKE_SYSROOT}")
  else()
    message(FATAL_ERROR
      "crashomon_store_sysroot_symbols: no sysroot specified and CMAKE_SYSROOT is not set.\n"
      "Pass SYSROOT <dir> or set CMAKE_SYSROOT in your toolchain file.")
  endif()

  if(ARG_STORE)
    set(_store "${ARG_STORE}")
  else()
    set(_store "${CRASHOMON_SYMBOL_STORE}")
  endif()

  if(ARG_SEARCH_PATHS)
    # CMake lists use semicolons — pass through directly to the -P script.
    set(_search "${ARG_SEARCH_PATHS}")
  else()
    set(_search "usr/lib")
  endif()

  if(NOT TARGET breakpad_dump_syms)
    message(FATAL_ERROR
      "crashomon_store_sysroot_symbols: dump_syms host binary not configured.\n"
      "See crashomon_store_symbols() error message for build instructions.")
  endif()

  add_custom_target(crashomon_sysroot_symbols
    COMMAND "${CMAKE_COMMAND}"
      "-DDUMP_SYMS=$<TARGET_FILE:breakpad_dump_syms>"
      "-DSYSROOT=${_sysroot}"
      "-DSTORE=${_store}"
      "-DSEARCH_PATHS=${_search}"
      -P "${_CRASHOMON_STORE_SYSROOT_IMPL}"
    COMMENT "Extracting sysroot symbols from ${_sysroot} → ${_store}"
    VERBATIM
  )
endfunction()
