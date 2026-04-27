include(GoogleTest)

add_executable(crashomon_daemon_tests
  test_disk_manager.cpp
  test_worker.cpp
)

target_link_libraries(crashomon_daemon_tests
  PRIVATE
    crashomon_warnings
    GTest::gmock_main
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

set_target_properties(crashomon_daemon_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

gtest_discover_tests(crashomon_daemon_tests)

# ── Minidump patcher tests ────────────────────────────────────────────────────

add_executable(test_minidump_patcher test_minidump_patcher.cpp)
target_link_libraries(test_minidump_patcher
  PRIVATE
    crashomon_patcher
    crashomon_warnings
    GTest::gtest_main
)
target_include_directories(test_minidump_patcher PRIVATE ${PROJECT_SOURCE_DIR})
if(crashomon_absl_SOURCE_DIR)
  target_include_directories(test_minidump_patcher SYSTEM PRIVATE "${crashomon_absl_SOURCE_DIR}")
endif()
set_target_properties(test_minidump_patcher PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

gtest_discover_tests(test_minidump_patcher)
