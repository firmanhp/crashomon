include(GoogleTest)

# ── Unit test binary ───────────────────────────────────────────────────────────
# Stub test files are added here as components are implemented.
# Each test file tests a single module and can be compiled independently.

set(TEST_SOURCES
  # Populated as components are built:
  # test_tombstone_formatter.cpp
  # test_disk_manager.cpp
  # test_minidump_reader.cpp
  # test_log_parser.cpp
)

if(TEST_SOURCES)
  add_executable(crashomon_tests ${TEST_SOURCES})
  target_link_libraries(crashomon_tests
    PRIVATE
      crashomon_warnings
      GTest::gtest_main
      # Component libraries added here as they are implemented
  )
  target_include_directories(crashomon_tests
    PRIVATE
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}
  )
  gtest_discover_tests(crashomon_tests)
endif()
