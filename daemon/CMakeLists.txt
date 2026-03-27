# crashomon-watcherd — inotify watcher daemon
# Watches for new minidumps, formats tombstones, logs to journald.

# TODO: Add Breakpad minidump-processor dependency when integrating.
# For now this is a stub that will be filled in during Phase 2.

add_executable(crashomon_watcherd
  main.cpp
  # tombstone_formatter.cpp
  # disk_manager.cpp
  # minidump_reader.cpp
)

target_link_libraries(crashomon_watcherd
  PRIVATE
    crashomon_warnings
    # breakpad_processor   (to be added)
    # systemd              (for sd_journal_send)
)

target_include_directories(crashomon_watcherd
  PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(crashomon_watcherd PROPERTIES OUTPUT_NAME crashomon-watcherd)
