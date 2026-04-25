include(GoogleTest)

# ── Daemon unit tests ──────────────────────────────────────────────────────────
# Tests for daemon internals (tombstone, disk_manager, minidump_reader,
# register_extract).  Does NOT link against the crashomon client library or
# crashpad_client, so there is no startup connection retry delay.

set(DAEMON_TEST_SOURCES
  ${PROJECT_SOURCE_DIR}/tombstone/test/test_tombstone_formatter.cpp
  ${PROJECT_SOURCE_DIR}/tombstone/test/test_minidump_reader.cpp
  ${PROJECT_SOURCE_DIR}/tombstone/test/test_register_extract.cpp
  ${PROJECT_SOURCE_DIR}/daemon/test/test_disk_manager.cpp
  ${PROJECT_SOURCE_DIR}/daemon/test/test_worker.cpp
)

add_executable(crashomon_daemon_tests ${DAEMON_TEST_SOURCES})

target_link_libraries(crashomon_daemon_tests
  PRIVATE
    crashomon_warnings
    crashomon_tombstone
    GTest::gtest_main
    breakpad_processor
    absl::status
    absl::statusor
    crashomon_spdlog
    Threads::Threads
)


# Modules under test compiled directly so tests can #include internal headers.
target_sources(crashomon_daemon_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/daemon/disk_manager.cpp
  ${PROJECT_SOURCE_DIR}/daemon/worker.cpp
)

target_include_directories(crashomon_daemon_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(crashomon_absl_SOURCE_DIR)
  target_include_directories(crashomon_daemon_tests SYSTEM PRIVATE "${crashomon_absl_SOURCE_DIR}")
endif()
if(crashomon_breakpad_SOURCE_DIR)
  target_include_directories(crashomon_daemon_tests SYSTEM PRIVATE "${crashomon_breakpad_SOURCE_DIR}/src")
endif()

set_target_properties(crashomon_daemon_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

# ── Client unit tests ──────────────────────────────────────────────────────────
# Tests for the crashomon client library (config resolution, tags, connect
# retry).  Links lib/crashomon.cpp and crashpad_client.
#
# CRASHOMON_TESTING_SKIP_AUTOINIT disables the AutoInit() constructor so the
# binary does not spend 3 s retrying the default socket path at startup.
# Tests that need the retry logic (test_connect_retry.cpp) call
# crashomon_init() directly and are not affected by this flag.

set(CLIENT_TEST_SOURCES
  ${PROJECT_SOURCE_DIR}/lib/test/test_crashomon.cpp
  ${PROJECT_SOURCE_DIR}/lib/test/test_tags.cpp
  ${PROJECT_SOURCE_DIR}/lib/test/test_connect_retry.cpp
  ${PROJECT_SOURCE_DIR}/lib/test/test_terminate_hooks.cpp
)

add_executable(crashomon_client_tests ${CLIENT_TEST_SOURCES})

target_compile_definitions(crashomon_client_tests PRIVATE CRASHOMON_TESTING_SKIP_AUTOINIT)

target_link_libraries(crashomon_client_tests
  PRIVATE
    crashomon_warnings
    GTest::gtest_main
    crashpad_client
    Threads::Threads
)

# lib/crashomon.cpp compiled directly so tests can #include internal headers.
# exception_capture.cpp is compiled with -fexceptions (see set_source_files_properties
# in lib/CMakeLists.txt); include it here so CaptureCurrentExceptionMessage resolves.
target_sources(crashomon_client_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/lib/crashomon.cpp
  ${PROJECT_SOURCE_DIR}/lib/exception_capture.cpp
)

set_source_files_properties(
  ${PROJECT_SOURCE_DIR}/lib/exception_capture.cpp
  PROPERTIES COMPILE_OPTIONS "-fexceptions"
)

target_include_directories(crashomon_client_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(crashomon_absl_SOURCE_DIR)
  target_include_directories(crashomon_client_tests SYSTEM PRIVATE "${crashomon_absl_SOURCE_DIR}")
endif()
# lib/crashomon.cpp and test_tags.cpp include Crashpad headers directly.
# Mark Crashpad's directories as SYSTEM so -Werror does not fire on third-party
# code (same approach as lib/CMakeLists.txt for crashomon_client targets).
if(TARGET crashpad_interface)
  target_include_directories(crashomon_client_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:crashpad_interface,INTERFACE_INCLUDE_DIRECTORIES>
  )
endif()

set_target_properties(crashomon_client_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

# ── Synthetic fixture generator ────────────────────────────────────────────────
# Generates three .dmp files at build time so all fixture tests run (no
# GTEST_SKIP) without requiring a running crashomon-watcherd daemon.

add_executable(gen_synthetic_fixtures gen_synthetic_fixtures.cpp)
target_compile_options(gen_synthetic_fixtures PRIVATE -w)  # third-party headers
target_link_libraries(gen_synthetic_fixtures
  PRIVATE
    breakpad_synth_minidump
    breakpad_processor
)
if(crashomon_breakpad_SOURCE_DIR)
  target_include_directories(gen_synthetic_fixtures SYSTEM PRIVATE
    "${crashomon_breakpad_SOURCE_DIR}/src")
endif()

# Run the generator at build time; outputs land in build/test/fixtures/.
set(FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")

add_custom_command(
  OUTPUT
    "${FIXTURE_DIR}/segfault.dmp"
    "${FIXTURE_DIR}/abort.dmp"
    "${FIXTURE_DIR}/multithread.dmp"
  COMMAND gen_synthetic_fixtures "${FIXTURE_DIR}"
  DEPENDS gen_synthetic_fixtures
  COMMENT "Generating synthetic minidump fixtures"
)

add_custom_target(synthetic_fixtures DEPENDS
  "${FIXTURE_DIR}/segfault.dmp"
  "${FIXTURE_DIR}/abort.dmp"
  "${FIXTURE_DIR}/multithread.dmp"
)

add_dependencies(crashomon_daemon_tests synthetic_fixtures)

gtest_discover_tests(crashomon_daemon_tests
  # Point to the build-time fixture directory so all fixture tests run.
  PROPERTIES
    ENVIRONMENT "CRASHOMON_FIXTURES_DIR=${FIXTURE_DIR}"
)

gtest_discover_tests(crashomon_client_tests)
