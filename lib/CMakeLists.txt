# libcrashomon — C++17 client library (C-compatible public header)
# Thin wrapper around sentry-native (Crashpad backend).
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
      sentry
  )
  target_include_directories(${target}
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}
  )
  # Position-independent code required for shared library
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

set_target_properties(crashomon_client PROPERTIES OUTPUT_NAME crashomon)
