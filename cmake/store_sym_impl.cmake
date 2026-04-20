# cmake/store_sym_impl.cmake — build-time cmake -P helper for crashomon_store_symbols().
#
# Invoked by the POST_BUILD command added by crashomon_store_symbols(). All inputs
# arrive as -D variables passed on the cmake command line so that generator
# expressions (evaluated by the build backend) can supply their values.
#
# Required -D variables:
#   DUMP_SYMS    — absolute path to the breakpad_dump_syms binary
#   BINARY       — absolute path to the ELF target just built
#   STORE        — root of the Breakpad symbol store (created if absent)
#   TARGET_NAME  — CMake target name (used to name the temp file uniquely)
#   BUILD_DIR    — CMAKE_BINARY_DIR (temp file lives here, not in STORE)
#
# Optional -D variables:
#   WITH_BINARY  — set to 1 to also copy the unstripped ELF into the store entry
#
# Store layout written:
#   <STORE>/<module_name>/<build_id>/<module_name>.sym
#   <STORE>/<module_name>/<build_id>/<module_name>       (only when WITH_BINARY=1)

# ── Step 1: write dump_syms output to a temp file ─────────────────────────────
# Use OUTPUT_FILE (not OUTPUT_VARIABLE) so large .sym files stream directly to
# disk — avoids loading potentially tens of MB into a CMake string variable.
# The temp file is named per-target so parallel builds never share a path.

set(_tmp "${BUILD_DIR}/.crashomon_sym_${TARGET_NAME}.sym.tmp")

# Pass the binary's directory as a second argument so dump_syms can follow
# .gnu_debuglink to a split .debug file (produced by objcopy --only-keep-debug).
# This allows dump_syms to read full DWARF info even when the binary itself
# has been stripped after the debug symbols were extracted.
cmake_path(GET BINARY PARENT_PATH _binary_dir)
execute_process(
  COMMAND "${DUMP_SYMS}" "${BINARY}" "${_binary_dir}"
  OUTPUT_FILE "${_tmp}"
  RESULT_VARIABLE _rc
  ERROR_VARIABLE  _err
)

if(NOT _rc EQUAL 0)
  file(REMOVE "${_tmp}")
  message(FATAL_ERROR
    "crashomon_store_symbols: dump_syms failed for ${BINARY} (exit ${_rc})\n${_err}")
endif()

# ── Step 2: parse the MODULE line ─────────────────────────────────────────────
# First line format: "MODULE <os> <arch> <build_id> <module_name>"
# Reading only one line avoids loading the full (potentially large) .sym file.

file(STRINGS "${_tmp}" _header LIMIT_COUNT 1)
string(REGEX MATCH "^MODULE [^ ]+ [^ ]+ ([^ ]+) (.+)" _m "${_header}")
set(_build_id    "${CMAKE_MATCH_1}")
set(_module_name "${CMAKE_MATCH_2}")

if(NOT _build_id OR NOT _module_name)
  file(REMOVE "${_tmp}")
  message(FATAL_ERROR
    "crashomon_store_symbols: unexpected MODULE line from dump_syms:\n"
    "  ${_header}\n"
    "Expected: MODULE <os> <arch> <build_id> <module_name>")
endif()

# ── Step 3: place the .sym file ───────────────────────────────────────────────
# file(RENAME) is atomic on Linux — a concurrent reader never sees a partial file.

set(_dest_dir "${STORE}/${_module_name}/${_build_id}")
file(MAKE_DIRECTORY "${_dest_dir}")
file(RENAME "${_tmp}" "${_dest_dir}/${_module_name}.sym")
message(STATUS
  "  symbols: ${_module_name}/${_build_id}/${_module_name}.sym")

# ── Step 4 (optional): copy the unstripped binary ─────────────────────────────
# When WITH_BINARY=1, the ELF is also stored alongside the .sym file.
#
# The binary is the unstripped build-tree artifact (POST_BUILD fires before any
# install-time stripping), so DWARF info is intact.

if(WITH_BINARY)
  file(COPY_FILE "${BINARY}" "${_dest_dir}/${_module_name}")
  message(STATUS
    "  binary:  ${_module_name}/${_build_id}/${_module_name}")
endif()
