# crashomon-watcherd — inotify watcher daemon
# Watches for new minidumps, formats tombstones, logs to stderr/journald.

add_executable(crashomon_watcherd
  main.cpp
  tombstone_formatter.cpp
  disk_manager.cpp
  minidump_reader.cpp
)

target_link_libraries(crashomon_watcherd
  PRIVATE
    crashomon_warnings
    breakpad_processor
    absl::status
    absl::statusor
    Threads::Threads
)

target_include_directories(crashomon_watcherd
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Abseil headers use __int128 (a GCC extension) and have internal sign-conversion
# issues that fire under our -Wpedantic and -Wsign-conversion flags.
# FetchContent before CMake 3.25 does not mark fetched-dep includes as SYSTEM.
# Adding the abseil source dir as SYSTEM here causes the compiler to use -isystem,
# which suppresses warnings from Abseil headers without affecting our own code.
if(abseil_SOURCE_DIR)
  target_include_directories(crashomon_watcherd SYSTEM PRIVATE "${abseil_SOURCE_DIR}")
endif()
# Breakpad headers use zero-size arrays (a C extension) and other non-pedantic
# constructs. Same SYSTEM workaround as for Abseil above.
if(breakpad_SOURCE_DIR)
  target_include_directories(crashomon_watcherd SYSTEM PRIVATE
    "${breakpad_SOURCE_DIR}/src")
endif()

set_target_properties(crashomon_watcherd PROPERTIES OUTPUT_NAME crashomon-watcherd)
