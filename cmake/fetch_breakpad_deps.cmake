# cmake/fetch_breakpad_deps.cmake — FetchContent population for Breakpad and its deps.
#
# Populates source trees for zlib, Breakpad, and linux-syscall-support (LSS).
# Does NOT create any CMake targets — consumers do that from the populated dirs.
# FetchContent_MakeAvailable is intentionally not used: it would process each
# dep's own CMakeLists.txt and create targets in the caller's scope, which the
# two consumers (CMakeLists.txt and dump_syms_host/) handle differently.
#
# After this file is included:
#   crashomon_zlib_SOURCE_DIR        — zlib source tree
#   crashomon_zlib_BINARY_DIR        — zlib FetchContent binary dir (may differ from
#                                      the binary dir used by add_subdirectory)
#   crashomon_breakpad_SOURCE_DIR    — Breakpad source tree
#   crashomon_breakpad_lss_SOURCE_DIR — LSS source tree

include(FetchContent)

# FetchContent_Populate(<name>) is intentionally used here (not MakeAvailable)
# so that each dep's CMakeLists.txt is NOT processed — the two consumers
# (CMakeLists.txt and dump_syms_host/) add subdirectories themselves with
# different options.  Suppress the CMP0169 deprecation warning that CMake 3.30+
# emits for this pattern.
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# ── zlib ──────────────────────────────────────────────────────────────────────

FetchContent_Declare(crashomon_zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG        v1.3.1
)
FetchContent_GetProperties(crashomon_zlib)
if(NOT crashomon_zlib_POPULATED)
  FetchContent_Populate(crashomon_zlib)
endif()

# ── Breakpad ──────────────────────────────────────────────────────────────────

FetchContent_Declare(crashomon_breakpad
  GIT_REPOSITORY https://github.com/google/breakpad.git
  GIT_TAG        v2024.02.16
)
FetchContent_GetProperties(crashomon_breakpad)
if(NOT crashomon_breakpad_POPULATED)
  FetchContent_Populate(crashomon_breakpad)
  # Downgrade caller_fp/caller_lr log severity from ERROR to INFO so they are
  # filtered by BPLOG_MINIMUM_SEVERITY=SEVERITY_ERROR (set in breakpad.cmake).
  # These fire on every scan-recovered frame when the target binary was compiled
  # without frame pointers (-fomit-frame-pointer, the -O2/-O3 default): the FP
  # unwind strategy fails, scan succeeds, but the stale x29 is never updated, so
  # the same FP address is retried — and fails — for each scanned frame.  The
  # behaviour is expected and harmless; the noise level scales with frame count.
  file(READ "${crashomon_breakpad_SOURCE_DIR}/src/processor/stackwalker_arm64.cc" _bp_src)
  string(REPLACE
    "BPLOG(ERROR) << \"Unable to read caller_fp"
    "BPLOG(INFO) << \"Unable to read caller_fp"
    _bp_src "${_bp_src}")
  string(REPLACE
    "BPLOG(ERROR) << \"Unable to read caller_lr"
    "BPLOG(INFO) << \"Unable to read caller_lr"
    _bp_src "${_bp_src}")
  file(WRITE "${crashomon_breakpad_SOURCE_DIR}/src/processor/stackwalker_arm64.cc" "${_bp_src}")
  unset(_bp_src)
endif()

# ── linux-syscall-support (LSS) ────────────────────────────────────────────────
# file_id.cc includes "third_party/lss/linux_syscall_support.h" relative to
# Breakpad's src/ directory.

FetchContent_Declare(crashomon_breakpad_lss
  GIT_REPOSITORY https://chromium.googlesource.com/linux-syscall-support.git
  GIT_TAG        v2024.02.01
)
FetchContent_GetProperties(crashomon_breakpad_lss)
if(NOT crashomon_breakpad_lss_POPULATED)
  FetchContent_Populate(crashomon_breakpad_lss)
endif()

set(_lss_dest "${crashomon_breakpad_SOURCE_DIR}/src/third_party/lss/linux_syscall_support.h")
if(NOT EXISTS "${_lss_dest}")
  file(MAKE_DIRECTORY "${crashomon_breakpad_SOURCE_DIR}/src/third_party/lss")
  file(CREATE_LINK
    "${crashomon_breakpad_lss_SOURCE_DIR}/linux_syscall_support.h"
    "${_lss_dest}"
    SYMBOLIC
  )
endif()
unset(_lss_dest)
