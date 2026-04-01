include(GoogleTest)

# ── Unit test binary ───────────────────────────────────────────────────────────
# Each test file tests a single module.

set(TEST_SOURCES
  test_crashomon.cpp
  test_tags.cpp
  test_tombstone_formatter.cpp
  test_disk_manager.cpp
  test_minidump_reader.cpp
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
  ${CMAKE_SOURCE_DIR}/daemon/disk_manager.cpp
  ${CMAKE_SOURCE_DIR}/lib/crashomon.cpp
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
# lib/crashomon.cpp and test_tags.cpp include Crashpad headers directly.
# Mark Crashpad's directories as SYSTEM so -Werror does not fire on third-party
# code (same approach as lib/CMakeLists.txt for crashomon_client targets).
if(TARGET crashpad_interface)
  target_include_directories(crashomon_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:crashpad_interface,INTERFACE_INCLUDE_DIRECTORIES>
  )
endif()

set_target_properties(crashomon_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

gtest_discover_tests(crashomon_tests
  # Pass the fixtures directory so test_minidump_reader.cpp can locate .dmp files.
  # Tests skip gracefully (GTEST_SKIP) when the fixture files are absent.
  PROPERTIES
    ENVIRONMENT "CRASHOMON_FIXTURES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
