include(GoogleTest)

# ── Tombstone unit tests ───────────────────────────────────────────────────────

add_executable(crashomon_tombstone_tests
  test_tombstone_formatter.cpp
  test_minidump_reader.cpp
  test_register_extract.cpp
)

target_link_libraries(crashomon_tombstone_tests
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

target_include_directories(crashomon_tombstone_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(crashomon_absl_SOURCE_DIR)
  target_include_directories(crashomon_tombstone_tests SYSTEM PRIVATE "${crashomon_absl_SOURCE_DIR}")
endif()
if(crashomon_breakpad_SOURCE_DIR)
  target_include_directories(crashomon_tombstone_tests SYSTEM PRIVATE
    "${crashomon_breakpad_SOURCE_DIR}/src")
endif()

set_target_properties(crashomon_tombstone_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

# ── Synthetic fixture generator ────────────────────────────────────────────────
# Generates three .dmp files at build time so all MinidumpReader fixture tests
# run without requiring a live crashomon-watcherd daemon.

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

add_dependencies(crashomon_tombstone_tests synthetic_fixtures)

gtest_discover_tests(crashomon_tombstone_tests
  PROPERTIES
    ENVIRONMENT "CRASHOMON_FIXTURES_DIR=${FIXTURE_DIR}"
)
