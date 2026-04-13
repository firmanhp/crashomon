include(GoogleTest)

# ── Unit test binary ───────────────────────────────────────────────────────────
# Each test file tests a single module.

set(TEST_SOURCES
  test_crashomon.cpp
  test_tags.cpp
  test_tombstone_formatter.cpp
  test_disk_manager.cpp
  test_minidump_reader.cpp
  test_register_extract.cpp
)

add_executable(crashomon_tests ${TEST_SOURCES})

target_link_libraries(crashomon_tests
  PRIVATE
    crashomon_warnings
    crashomon_tombstone
    GTest::gtest_main
    breakpad_processor
    absl::status
    absl::statusor
    Threads::Threads
    crashpad_client
)

# Modules under test — compiled directly into the test binary so tests
# can #include internal headers without a separate library target.
target_sources(crashomon_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/daemon/disk_manager.cpp
  ${PROJECT_SOURCE_DIR}/lib/crashomon.cpp
)

target_include_directories(crashomon_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Same Abseil SYSTEM-include workaround as in daemon/CMakeLists.txt.
if(abseil_SOURCE_DIR)
  target_include_directories(crashomon_tests SYSTEM PRIVATE "${abseil_SOURCE_DIR}")
endif()
if(breakpad_SOURCE_DIR)
  target_include_directories(crashomon_tests SYSTEM PRIVATE "${breakpad_SOURCE_DIR}/src")
endif()
# lib/crashomon.cpp and test_tags.cpp include Crashpad headers directly.
# Mark Crashpad's directories as SYSTEM so -Werror does not fire on third-party
# code (same approach as lib/CMakeLists.txt for crashomon_client targets).
if(TARGET crashpad_interface)
  target_include_directories(crashomon_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:crashpad_interface,INTERFACE_INCLUDE_DIRECTORIES>
  )
endif()

set_target_properties(crashomon_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

# ── Synthetic fixture generator ────────────────────────────────────────────────
# Generates three .dmp files at build time so all 18 fixture tests run (no
# GTEST_SKIP) without requiring a running crashomon-watcherd daemon.

add_executable(gen_synthetic_fixtures gen_synthetic_fixtures.cpp)
target_compile_options(gen_synthetic_fixtures PRIVATE -w)  # third-party headers
target_link_libraries(gen_synthetic_fixtures
  PRIVATE
    breakpad_synth_minidump
    breakpad_processor
)
if(breakpad_SOURCE_DIR)
  target_include_directories(gen_synthetic_fixtures SYSTEM PRIVATE
    "${breakpad_SOURCE_DIR}/src")
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

add_dependencies(crashomon_tests synthetic_fixtures)

gtest_discover_tests(crashomon_tests
  # Point to the build-time fixture directory so all 18 fixture tests run.
  PROPERTIES
    ENVIRONMENT "CRASHOMON_FIXTURES_DIR=${FIXTURE_DIR}"
)
