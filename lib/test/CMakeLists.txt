include(GoogleTest)

add_executable(crashomon_client_tests
  test_crashomon.cpp
  test_tags.cpp
  test_connect_retry.cpp
  test_terminate_hooks.cpp
)

target_compile_definitions(crashomon_client_tests PRIVATE CRASHOMON_TESTING_SKIP_AUTOINIT)

target_link_libraries(crashomon_client_tests
  PRIVATE
    crashomon_warnings
    GTest::gtest_main
    crashpad_client
    Threads::Threads
)

# lib/crashomon.cpp compiled directly so tests can #include internal headers.
# exception_capture.cpp is compiled with -fexceptions (see lib/CMakeLists.txt);
# include it here so CaptureCurrentExceptionMessage resolves at link time.
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
# Mark Crashpad's directories as SYSTEM so -Werror does not fire on third-party code.
if(TARGET crashpad_interface)
  target_include_directories(crashomon_client_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:crashpad_interface,INTERFACE_INCLUDE_DIRECTORIES>
  )
endif()

set_target_properties(crashomon_client_tests PROPERTIES CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}")

gtest_discover_tests(crashomon_client_tests)
