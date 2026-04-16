cmake_minimum_required(VERSION 3.24)
project(crashomon VERSION 0.1.0 LANGUAGES C CXX)

# ── Language standards ─────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Default build flags ────────────────────────────────────────────────────────
# Single build type: optimized with debug info.
# No separate Debug/Release — sanitizers are opt-in toggles below.
add_compile_options(-O3 -g)
# No exceptions in C++ code. All error handling uses absl::Status / absl::StatusOr.
# Public interfaces must be freestanding — absl types must not appear in public headers.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Sanitizer options (opt-in) ─────────────────────────────────────────────────
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)
option(ENABLE_ASAN  "Enable AddressSanitizer"           OFF)

if(ENABLE_UBSAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_UBSAN and ENABLE_TSAN cannot be used together.")
endif()
if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN cannot be used together.")
endif()

if(ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# ── clang-tidy (opt-in) ────────────────────────────────────────────────────────
# Applied per-target (not globally) so FetchContent dependencies are never linted.
# Use: set_target_properties(<target> PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")
option(ENABLE_TESTS      "Build tests, examples, and benchmarks" OFF)
option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
if(ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES "clang-tidy" REQUIRED)
  set(CRASHOMON_CLANG_TIDY "${CLANG_TIDY_EXE}")
else()
  set(CRASHOMON_CLANG_TIDY "")
endif()

# ── Install options ────────────────────────────────────────────────────────────
# Default ON when crashomon is the top-level project so `cmake --install` works
# out of the box. OFF when consumed via add_subdirectory or FetchContent so that
# embedders do not inherit install rules they did not ask for.
# Mirrors the pattern used by Crashpad (CRASHPAD_ENABLE_INSTALL), abseil,
# googletest, and google-benchmark.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(_CRASHOMON_INSTALL_DEFAULT ON)
else()
  set(_CRASHOMON_INSTALL_DEFAULT OFF)
endif()

option(CRASHOMON_ENABLE_INSTALL   "Generate crashomon install() rules"    ${_CRASHOMON_INSTALL_DEFAULT})
option(CRASHOMON_INSTALL_CLIENT   "Install client library + header"        ON)
option(CRASHOMON_INSTALL_WATCHERD "Install watcherd daemon + dump_syms"    ON)
option(CRASHOMON_INSTALL_ANALYZE  "Install analyze CLI + stackwalk"        ON)
# Separate build toggle: build the analyze tools (minidump_stackwalk) without
# installing them. Useful when consuming crashomon as a subproject but still
# needing the tool at build time.
option(CRASHOMON_BUILD_ANALYZE    "Build analyze tools without installing" OFF)

# ── Compiler warnings — applied to OUR targets only via interface library ──────
# Third-party deps (Crashpad, GoogleTest) are fetched separately and must
# NOT inherit -Werror. We use target_link_libraries(our_target PRIVATE crashomon_warnings)
# on each of our own targets.
add_library(crashomon_warnings INTERFACE)
target_compile_options(crashomon_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Werror
  -Wshadow -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wformat-security
  -fstack-protector-strong
  # Crashpad's compat/linux/ shims use #include_next (a GCC extension).
  # -Wpedantic enables -Wgnu-include-next; suppress it so Crashpad compat
  # headers do not fire -Werror in our translation units.  Our own code
  # never uses #include_next, so this suppression is safe.
  -Wno-gnu-include-next
)

# ── Dependencies ───────────────────────────────────────────────────────────────
include(FetchContent)

# zlib — required by Crashpad and Breakpad.
# Must be configured BEFORE Crashpad so its find_package(ZLIB REQUIRED) succeeds.
# We create a ZLIB::ZLIB alias and set FindZLIB cache vars so downstream find_package
# calls short-circuit and use our built-from-source version.
FetchContent_Declare(crashomon_zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG        v1.3.1
)
FetchContent_GetProperties(crashomon_zlib)
if(NOT crashomon_zlib_POPULATED)
  FetchContent_Populate(crashomon_zlib)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${crashomon_zlib_SOURCE_DIR}" "${crashomon_zlib_BINARY_DIR}" EXCLUDE_FROM_ALL)
  target_compile_options(zlibstatic PRIVATE -w)
  # Pin zlibstatic's output to its own build dir regardless of any
  # CMAKE_ARCHIVE_OUTPUT_DIRECTORY set by a parent project. ZLIB::ZLIB's
  # IMPORTED_LOCATION hardcodes this path and must match the actual output.
  set_target_properties(zlibstatic PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${crashomon_zlib_BINARY_DIR}"
  )
endif()
# Create ZLIB::ZLIB as an IMPORTED target (not ALIAS) so it is valid in CMake export sets.
# ALIAS targets of non-IMPORTED builds cause export set validation failures (CMake requires
# all transitive build-target deps to be exported). IMPORTED targets are self-describing.
# We add_dependencies(... zlibstatic) after Crashpad's add_subdirectory to ensure
# correct build ordering.
if(NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB IMPORTED STATIC GLOBAL)
  set_target_properties(ZLIB::ZLIB PROPERTIES
    IMPORTED_LOCATION "${crashomon_zlib_BINARY_DIR}/libz.a"
    INTERFACE_INCLUDE_DIRECTORIES "${crashomon_zlib_SOURCE_DIR};${crashomon_zlib_BINARY_DIR}"
  )
endif()
set(ZLIB_FOUND       TRUE          CACHE BOOL   "zlib found (FetchContent)" FORCE)
set(ZLIB_INCLUDE_DIR "${crashomon_zlib_SOURCE_DIR};${crashomon_zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY     "${crashomon_zlib_BINARY_DIR}/libz.a" CACHE FILEPATH "" FORCE)
set(ZLIB_LIBRARIES   "${crashomon_zlib_BINARY_DIR}/libz.a" CACHE STRING   "" FORCE)
set(ZLIB_VERSION_STRING "1.3.1"   CACHE STRING  "" FORCE)

# CURL stub — Crashpad's util unconditionally compiles http_transport_libcurl.cc on Linux,
# which includes <curl/curl.h>. With no upload URL configured, this code is never called at
# runtime. See cmake/libcurl_stub.cmake and cmake/curl_stub/curl.h for details.
include(cmake/libcurl_stub.cmake)

# Crashpad — out-of-process crash capture.
# sentry-native's repo vendors a complete CMake build of Crashpad at external/crashpad/.
# We fetch the sentry-native source but add_subdirectory only the Crashpad subtree,
# bypassing the sentry wrapper entirely. No sentry targets are built or linked.
FetchContent_Declare(crashomon_crashpad
  GIT_REPOSITORY https://github.com/getsentry/sentry-native.git
  GIT_TAG        0.7.17
)
FetchContent_GetProperties(crashomon_crashpad)
if(NOT crashomon_crashpad_POPULATED)
  FetchContent_Populate(crashomon_crashpad)
endif()

# Crashpad's CMakeLists.txt sets CMAKE_CXX_STANDARD 20 in its own directory scope —
# it does not pollute our project's global C++20 setting.
set(CRASHPAD_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
set(CRASHPAD_ENABLE_INSTALL_DEV OFF CACHE BOOL "" FORCE)
# Build all Crashpad static libs with -fPIC so they can be linked into libcrashomon.so.
# Scoped before add_subdirectory: all targets created in that subdirectory inherit this.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(
  "${crashomon_crashpad_SOURCE_DIR}/external/crashpad"
  "${CMAKE_BINARY_DIR}/crashpad_build"
  EXCLUDE_FROM_ALL  # suppress Crashpad's own install() rules; we install only what we need
)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# Mark Crashpad's include directories as SYSTEM so that our -Werror flags don't fire on
# Crashpad headers when they are included in our own translation units.
if(TARGET crashpad_interface)
  get_target_property(_cp_inc crashpad_interface INTERFACE_INCLUDE_DIRECTORIES)
  if(_cp_inc)
    set_property(TARGET crashpad_interface APPEND PROPERTY
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_cp_inc}")
  endif()
  unset(_cp_inc)
endif()

# Suppress warnings on all Crashpad targets (third-party code, exempt from -Werror).
# Skip INTERFACE targets — they have no sources to compile, only propagated properties.
foreach(_cp_tgt IN ITEMS
    crashpad_client crashpad_util crashpad_handler crashpad_handler_lib
    crashpad_minidump crashpad_snapshot crashpad_compat crashpad_tools
    mini_chromium crashpad_zlib)
  if(TARGET ${_cp_tgt})
    get_target_property(_cp_tgt_type ${_cp_tgt} TYPE)
    if(NOT _cp_tgt_type STREQUAL "INTERFACE_LIBRARY")
      target_compile_options(${_cp_tgt} PRIVATE -w)
    endif()
  endif()
endforeach()
unset(_cp_tgt_type)

# Establish build ordering: zlibstatic must be built before crashpad_handler links.
if(TARGET crashpad_handler)
  add_dependencies(crashpad_handler zlibstatic)
endif()

# GoogleTest for unit tests (only when ENABLE_TESTS=ON)
if(ENABLE_TESTS)
  FetchContent_Declare(GTest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    OVERRIDE_FIND_PACKAGE  # intercept find_package(GTest) to avoid config-file ALIAS errors
  )
  # Prevent GoogleTest from overriding our compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(GTest)
endif()

# Abseil — internal error handling (absl::Status, absl::StatusOr).
# Internal only: absl types must NOT appear in public/cross-library interfaces.
# Only link the specific targets used per component to avoid binary bloat.
FetchContent_Declare(crashomon_absl
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        20240722.0
)
# Only populate if no caller has already provided absl (e.g. via Conan, vcpkg,
# or their own FetchContent). absl::base is present in every absl installation
# and serves as the canonical existence check.
if(NOT TARGET absl::base)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  set(ABSL_BUILD_TESTING      OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING            OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(crashomon_absl)
endif()

# Abseil headers use __int128 (a GCC extension) and have internal sign-conversion
# issues. FetchContent before CMake 3.25 does not mark fetched deps as SYSTEM
# includes, so our -Wpedantic/-Wsign-conversion would fire on Abseil headers when
# our own code includes them.
#
# Fix: propagate crashomon_absl_SOURCE_DIR as INTERFACE_SYSTEM_INCLUDE_DIRECTORIES on all
# absl targets so any downstream target that links absl gets SYSTEM includes.
# This suppresses warnings only from Abseil headers — not from our own code.
if(crashomon_absl_SOURCE_DIR)
  get_property(_absl_targets DIRECTORY "${crashomon_absl_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_tgt IN LISTS _absl_targets)
    if(TARGET ${_tgt})
      get_target_property(_type ${_tgt} TYPE)
      if(NOT _type STREQUAL "INTERFACE_LIBRARY")
        set_property(TARGET ${_tgt} APPEND PROPERTY
          INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${crashomon_absl_SOURCE_DIR}")
      else()
        # INTERFACE targets may have their own include dirs — mark those too.
        get_target_property(_inc ${_tgt} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
          set_property(TARGET ${_tgt} APPEND PROPERTY
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
        endif()
      endif()
    endif()
  endforeach()
endif()

# Breakpad — minidump-processor library + dump_syms + minidump_stackwalk tools.
# Breakpad uses GN/autotools; we define our own CMake targets (vcpkg-style).

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
  execute_process(
    COMMAND sed -i
      -e "s|BPLOG(ERROR) << \"Unable to read caller_fp|BPLOG(INFO) << \"Unable to read caller_fp|g"
      -e "s|BPLOG(ERROR) << \"Unable to read caller_lr|BPLOG(INFO) << \"Unable to read caller_lr|g"
      "${crashomon_breakpad_SOURCE_DIR}/src/processor/stackwalker_arm64.cc"
  )
endif()

# LSS header needed by dump_syms (file_id.cc: #include "third_party/lss/linux_syscall_support.h")
FetchContent_Declare(crashomon_breakpad_lss
  GIT_REPOSITORY https://chromium.googlesource.com/linux-syscall-support.git
  GIT_TAG        v2024.02.01
)
FetchContent_GetProperties(crashomon_breakpad_lss)
if(NOT crashomon_breakpad_lss_POPULATED)
  FetchContent_Populate(crashomon_breakpad_lss)
endif()
# Create the directory and symlink where Breakpad expects the LSS header
file(MAKE_DIRECTORY "${crashomon_breakpad_SOURCE_DIR}/src/third_party/lss")
file(CREATE_LINK
  "${crashomon_breakpad_lss_SOURCE_DIR}/linux_syscall_support.h"
  "${crashomon_breakpad_SOURCE_DIR}/src/third_party/lss/linux_syscall_support.h"
  SYMBOLIC
)

include(cmake/breakpad.cmake)
include(cmake/crashomon_symbols.cmake)

# spdlog — structured logging for crashomon-watcherd.
# Vendored as headers in third_party/spdlog/ (v1.14.1) to avoid FetchContent
# namespace conflicts when crashomon is consumed as a subdirectory. Consumers
# that provide their own spdlog are unaffected: crashomon's internal target is
# distinct and does not interfere with any find_package(spdlog) in the outer build.
add_library(crashomon_spdlog INTERFACE)
target_include_directories(crashomon_spdlog SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include"
)
# SPDLOG_NO_EXCEPTIONS: matches our global -fno-exceptions; replaces throw/catch
# with abort paths throughout spdlog's headers.
target_compile_definitions(crashomon_spdlog INTERFACE SPDLOG_NO_EXCEPTIONS=1)

# Google Benchmark (only when ENABLE_TESTS=ON)
if(ENABLE_TESTS)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    OVERRIDE_FIND_PACKAGE  # intercept find_package(benchmark) to avoid config-file ALIAS errors
  )
  set(BENCHMARK_ENABLE_TESTING    OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# ── Subdirectories ─────────────────────────────────────────────────────────────
add_subdirectory(lib)
add_subdirectory(tombstone)
add_subdirectory(daemon)

if(ENABLE_TESTS)
  add_subdirectory(examples)
  enable_testing()
  add_subdirectory(test)

  # Python tests via pytest — wired into ctest so `ctest` covers the full suite.
  # Prefer the project's .venv interpreter (populated by uv), then fall back to
  # whatever Python3 CMake finds.  Gate on interpreter having pytest installed.
  find_program(VENV_PYTHON NAMES python python3
    PATHS "${PROJECT_SOURCE_DIR}/.venv/bin"
    NO_DEFAULT_PATH
  )
  if(NOT VENV_PYTHON)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(VENV_PYTHON "${Python3_EXECUTABLE}")
  endif()
  if(VENV_PYTHON)
    execute_process(
      COMMAND ${VENV_PYTHON} -c "import pytest"
      RESULT_VARIABLE _pytest_found
      OUTPUT_QUIET ERROR_QUIET
    )
    if(_pytest_found EQUAL 0)
      add_test(NAME python_tests
        COMMAND ${VENV_PYTHON} -m pytest --tb=short -q
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      )
      set_tests_properties(python_tests PROPERTIES
        ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}"
        LABELS "python"
      )
    endif()
  endif()
  add_subdirectory(bench)
endif()

# ── Install ────────────────────────────────────────────────────────────────────
# CRASHOMON_ENABLE_INSTALL is ON by default only when crashomon is the top-level
# project. Per-component flags further narrow what gets installed.
# Selective install: cmake --install build --component <client|watcherd|analyze>
if(CRASHOMON_ENABLE_INSTALL)
  include(GNUInstallDirs)

  if(CRASHOMON_INSTALL_CLIENT)
    install(TARGETS crashomon_client
      LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}  COMPONENT client
      ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}  COMPONENT client
      RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}  COMPONENT client
    )
    install(FILES lib/crashomon.h
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/crashomon
      COMPONENT client)
  endif()

  if(CRASHOMON_INSTALL_WATCHERD)
    install(TARGETS crashomon_watcherd
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT watcherd)
    install(PROGRAMS $<TARGET_FILE:breakpad_dump_syms>
      DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/crashomon
      COMPONENT watcherd)
  endif()

  if(CRASHOMON_INSTALL_ANALYZE)
    install(PROGRAMS tools/analyze/crashomon-analyze
      DESTINATION ${CMAKE_INSTALL_BINDIR}
      COMPONENT analyze)
    install(PROGRAMS $<TARGET_FILE:minidump_stackwalk>
      DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/crashomon
      COMPONENT analyze)
  endif()
endif()

# ── Developer shortcut: run_analyze ───────────────────────────────────────────
# Builds minidump_stackwalk then runs crashomon-analyze, auto-injecting
# --stackwalk-binary so the freshly built binary is used automatically.
# Additional flags are forwarded via the ANALYZE_ARGS cache variable.
#
# Quick start:
#   cmake -B build -DANALYZE_ARGS="--minidump=crash.dmp --store=./syms"
#   cmake --build build --target run_analyze
#
# Only defined when crashomon is the top-level project (not in FetchContent).
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND TARGET minidump_stackwalk)
  set(ANALYZE_ARGS "" CACHE STRING
    "Arguments forwarded to crashomon-analyze by the run_analyze target \
(e.g. \"--minidump=crash.dmp --store=./syms\")")

  # Prefer the project's .venv interpreter (populated by uv), then fall back
  # to whatever Python3 CMake finds — same search order as the test block.
  find_program(_run_analyze_python NAMES python python3
    PATHS "${PROJECT_SOURCE_DIR}/.venv/bin"
    NO_DEFAULT_PATH
  )
  if(NOT _run_analyze_python)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    set(_run_analyze_python "${Python3_EXECUTABLE}")
  endif()

  separate_arguments(_run_analyze_args NATIVE_COMMAND "${ANALYZE_ARGS}")

  add_custom_target(run_analyze
    COMMAND
      ${CMAKE_COMMAND} -E env "PYTHONPATH=${PROJECT_SOURCE_DIR}"
      "${_run_analyze_python}"
      "${PROJECT_SOURCE_DIR}/tools/analyze/crashomon-analyze"
      "--stackwalk-binary=$<TARGET_FILE:minidump_stackwalk>"
      ${_run_analyze_args}
    DEPENDS minidump_stackwalk
    USES_TERMINAL
    COMMENT "crashomon-analyze (ANALYZE_ARGS=${ANALYZE_ARGS})"
  )

  unset(_run_analyze_python)
  unset(_run_analyze_args)
endif()
