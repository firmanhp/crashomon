# crashomon-watcherd — inotify watcher daemon
# Watches for new minidumps, formats tombstones, logs to stderr/journald.

add_executable(crashomon_watcherd
  main.cpp
  crash_socket.cpp
  disk_manager.cpp
  watcher.cpp
  worker.cpp
)

target_link_libraries(crashomon_watcherd
  PRIVATE
    crashomon_warnings
    crashomon_tombstone
    crashpad_handler_lib
    crashpad_tools
    crashpad_client
    crashomon_spdlog
    Threads::Threads
)


target_include_directories(crashomon_watcherd
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Abseil headers use __int128 (a GCC extension) and have internal sign-conversion
# issues that fire under our -Wpedantic and -Wsign-conversion flags.
# FetchContent before CMake 3.25 does not mark fetched-dep includes as SYSTEM.
# Adding the abseil source dir as SYSTEM here causes the compiler to use -isystem,
# which suppresses warnings from Abseil headers without affecting our own code.
if(absl_SOURCE_DIR)
  target_include_directories(crashomon_watcherd SYSTEM PRIVATE "${absl_SOURCE_DIR}")
endif()
# Breakpad headers use zero-size arrays (a C extension) and other non-pedantic
# constructs. Same SYSTEM workaround as for Abseil above.
if(crashomon_breakpad_SOURCE_DIR)
  target_include_directories(crashomon_watcherd SYSTEM PRIVATE
    "${crashomon_breakpad_SOURCE_DIR}/src")
endif()
# Crashpad headers: mark as SYSTEM to suppress warnings from third-party code.
if(TARGET crashpad_interface)
  get_target_property(_cp_inc crashpad_interface INTERFACE_INCLUDE_DIRECTORIES)
  if(_cp_inc)
    target_include_directories(crashomon_watcherd SYSTEM PRIVATE ${_cp_inc})
  endif()
  unset(_cp_inc)
endif()

# sd_notify watchdog support — optional, falls back gracefully if libsystemd is absent.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBSYSTEMD QUIET libsystemd)
  if(LIBSYSTEMD_FOUND)
    target_compile_definitions(crashomon_watcherd PRIVATE HAVE_LIBSYSTEMD=1)
    target_link_libraries(crashomon_watcherd PRIVATE ${LIBSYSTEMD_LIBRARIES})
    # SYSTEM suppresses -Werror on systemd headers (not our code).
    target_include_directories(crashomon_watcherd SYSTEM PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})
  endif()
endif()

set_target_properties(crashomon_watcherd PROPERTIES
  OUTPUT_NAME crashomon-watcherd
  CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}"
)
