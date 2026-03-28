include(GoogleTest)

# ── Unit test binary ───────────────────────────────────────────────────────────
# Each test file tests a single module.

set(TEST_SOURCES
  test_tombstone_formatter.cpp
  test_disk_manager.cpp
  test_log_parser.cpp
  # test_minidump_reader.cpp  — Phase 5: requires fixture .dmp files
)

add_executable(crashomon_tests ${TEST_SOURCES})

target_link_libraries(crashomon_tests
  PRIVATE
    crashomon_warnings
    GTest::gtest_main
    breakpad_processor
    absl::status
    absl::statusor
    Threads::Threads
)

# Daemon modules under test — compiled directly into the test binary so tests
# can #include internal headers without a separate library target.
target_sources(crashomon_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/daemon/tombstone_formatter.cpp
  ${CMAKE_SOURCE_DIR}/daemon/disk_manager.cpp
  ${CMAKE_SOURCE_DIR}/daemon/minidump_reader.cpp
  ${CMAKE_SOURCE_DIR}/tools/analyze/log_parser.cpp
)

target_include_directories(crashomon_tests
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Same Abseil SYSTEM-include workaround as in daemon/CMakeLists.txt.
if(abseil_SOURCE_DIR)
  target_include_directories(crashomon_tests SYSTEM PRIVATE "${abseil_SOURCE_DIR}")
endif()
if(breakpad_SOURCE_DIR)
  target_include_directories(crashomon_tests SYSTEM PRIVATE "${breakpad_SOURCE_DIR}/src")
endif()

gtest_discover_tests(crashomon_tests)
