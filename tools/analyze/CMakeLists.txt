# crashomon-analyze — CLI symbolication tool
# Symbolicates a minidump or tombstone text against a symbol store.

add_executable(crashomon_analyze
  main.cpp
  # log_parser.cpp
  # minidump_analyzer.cpp
  # symbolizer.cpp
)

target_link_libraries(crashomon_analyze
  PRIVATE
    crashomon_warnings
    breakpad_processor
    absl::status
    absl::statusor
)

target_include_directories(crashomon_analyze
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(crashomon_analyze PROPERTIES OUTPUT_NAME crashomon-analyze)
