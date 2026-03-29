# libcrashomon — C++17 client library (C-compatible public header)
# Thin wrapper around Crashpad for out-of-process crash capture.
# Supports LD_PRELOAD (via constructor/destructor) and explicit linking.

add_library(crashomon_client SHARED
  crashomon.cpp
)

add_library(crashomon_client_static STATIC
  crashomon.cpp
)
set_target_properties(crashomon_client_static PROPERTIES OUTPUT_NAME crashomon)

foreach(target crashomon_client crashomon_client_static)
  target_link_libraries(${target}
    PRIVATE
      crashomon_warnings
      crashpad_client
  )
  target_include_directories(${target}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}
  )
  # Crashpad headers use #include_next, sign-conversion, and other extensions that
  # trigger our -Werror flags. Mark Crashpad's include directories as SYSTEM so the
  # compiler treats them as third-party headers and suppresses those warnings.
  target_include_directories(${target} SYSTEM PRIVATE
    $<TARGET_PROPERTY:crashpad_interface,INTERFACE_INCLUDE_DIRECTORIES>
  )
  # Position-independent code required for shared library
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

set_target_properties(crashomon_client PROPERTIES OUTPUT_NAME crashomon)
