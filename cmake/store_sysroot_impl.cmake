# cmake/store_sysroot_impl.cmake — build-time cmake -P helper for sysroot symbol extraction.
#
# Scans a cross-compilation sysroot for shared libraries, runs dump_syms on
# each, and stores the resulting .sym files in the Breakpad symbol store layout.
#
# Required -D variables:
#   DUMP_SYMS    — absolute path to the breakpad_dump_syms binary
#   SYSROOT      — absolute path to the sysroot root (e.g. .../aarch64-.../sysroot)
#   STORE        — root of the Breakpad symbol store
#   SEARCH_PATHS — semicolon-separated list of relative paths under SYSROOT to scan
#                  (e.g. "usr/lib;lib64")

# ── Step 1: collect unique .so files across all search paths ─────────────────
# Resolve symlinks to avoid processing the same physical file twice.
# (Buildroot merged-usr: lib/ -> usr/lib, lib64/ -> lib -> usr/lib)

set(_seen "")  # list of resolved real paths already processed
set(_candidates "")

foreach(_rel IN LISTS SEARCH_PATHS)
  set(_dir "${SYSROOT}/${_rel}")
  if(NOT IS_DIRECTORY "${_dir}")
    continue()
  endif()

  file(GLOB _libs "${_dir}/*.so" "${_dir}/*.so.*")
  foreach(_lib IN LISTS _libs)
    file(REAL_PATH "${_lib}" _real)
    if(_real IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_real}")
    list(APPEND _candidates "${_real}")
  endforeach()
endforeach()

list(LENGTH _candidates _count)
if(_count EQUAL 0)
  message(WARNING "crashomon_store_sysroot_symbols: no .so files found under ${SYSROOT}")
  return()
endif()

message(STATUS "crashomon_store_sysroot_symbols: found ${_count} libraries to process")

# ── Step 2: run dump_syms on each library and store results ──────────────────

set(_stored 0)
set(_skipped 0)

foreach(_lib IN LISTS _candidates)
  cmake_path(GET _lib FILENAME _basename)

  # Use a per-library temp file to avoid collisions.
  string(REGEX REPLACE "[^a-zA-Z0-9._-]" "_" _safe "${_basename}")
  set(_tmp "${STORE}/.sysroot_tmp_${_safe}.sym")

  # dump_syms may fail on libraries without debug info or unsupported formats.
  # Skip silently — the user opted in, failures on individual libs are expected.
  execute_process(
    COMMAND "${DUMP_SYMS}" "${_lib}"
    OUTPUT_FILE "${_tmp}"
    RESULT_VARIABLE _rc
    ERROR_QUIET
  )

  if(NOT _rc EQUAL 0)
    file(REMOVE "${_tmp}")
    math(EXPR _skipped "${_skipped} + 1")
    continue()
  endif()

  # Parse MODULE line: "MODULE <os> <arch> <build_id> <module_name>"
  file(STRINGS "${_tmp}" _header LIMIT_COUNT 1)
  string(REGEX MATCH "^MODULE [^ ]+ [^ ]+ ([^ ]+) (.+)" _m "${_header}")
  set(_build_id    "${CMAKE_MATCH_1}")
  set(_module_name "${CMAKE_MATCH_2}")

  if(NOT _build_id OR NOT _module_name)
    file(REMOVE "${_tmp}")
    math(EXPR _skipped "${_skipped} + 1")
    continue()
  endif()

  # Check for placeholder build IDs (e.g. libraries stripped with --strip-all
  # that lost their .note.gnu.build-id section).
  string(LENGTH "${_build_id}" _id_len)
  if(_id_len LESS 8)
    file(REMOVE "${_tmp}")
    message(STATUS "  skip (no build ID): ${_basename}")
    math(EXPR _skipped "${_skipped} + 1")
    continue()
  endif()

  set(_dest_dir "${STORE}/${_module_name}/${_build_id}")
  file(MAKE_DIRECTORY "${_dest_dir}")
  file(RENAME "${_tmp}" "${_dest_dir}/${_module_name}.sym")
  message(STATUS "  stored: ${_module_name}/${_build_id}/${_module_name}.sym")
  math(EXPR _stored "${_stored} + 1")
endforeach()

message(STATUS "crashomon_store_sysroot_symbols: ${_stored} stored, ${_skipped} skipped")
