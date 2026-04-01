# crashomon-analyze — CLI symbolication tool
# Symbolicates a minidump or tombstone text against a symbol store.

add_executable(crashomon_analyze
  main.cpp
  log_parser.cpp
  minidump_analyzer.cpp
  symbolizer.cpp
)

target_link_libraries(crashomon_analyze
  PRIVATE
    crashomon_warnings
    crashomon_tombstone
    breakpad_processor
    absl::status
    absl::statusor
)

target_include_directories(crashomon_analyze
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(abseil_SOURCE_DIR)
  target_include_directories(crashomon_analyze SYSTEM PRIVATE "${abseil_SOURCE_DIR}")
endif()
if(breakpad_SOURCE_DIR)
  target_include_directories(crashomon_analyze SYSTEM PRIVATE "${breakpad_SOURCE_DIR}/src")
endif()

set_target_properties(crashomon_analyze PROPERTIES
  OUTPUT_NAME crashomon-analyze
  CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}"
)
