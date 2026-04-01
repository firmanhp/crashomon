# crashomon_tombstone — minidump reader + tombstone formatter library
# Shared by crashomon-watcherd, crashomon-analyze, and unit tests.

add_library(crashomon_tombstone STATIC
  minidump_reader.cpp
  tombstone_formatter.cpp
)

target_link_libraries(crashomon_tombstone
  PUBLIC
    absl::status
    absl::statusor
  PRIVATE
    crashomon_warnings
    breakpad_processor
)

target_include_directories(crashomon_tombstone
  PUBLIC
    ${CMAKE_SOURCE_DIR}
)

# Abseil headers use __int128 (a GCC extension) and have internal sign-conversion
# issues that fire under our -Wpedantic and -Wsign-conversion flags.
if(abseil_SOURCE_DIR)
  target_include_directories(crashomon_tombstone SYSTEM PRIVATE "${abseil_SOURCE_DIR}")
endif()
# Breakpad headers use zero-size arrays (a C extension) and other non-pedantic
# constructs.
if(breakpad_SOURCE_DIR)
  target_include_directories(crashomon_tombstone SYSTEM PRIVATE
    "${breakpad_SOURCE_DIR}/src")
endif()

set_target_properties(crashomon_tombstone PROPERTIES
  CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}"
)
