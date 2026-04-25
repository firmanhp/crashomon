# libcrashomon — C++17 client library (C-compatible public header)
# Thin wrapper around Crashpad for out-of-process crash capture.
# Supports LD_PRELOAD (via constructor/destructor) and explicit linking.

add_library(crashomon_client SHARED
  crashomon.cpp
  exception_capture.cpp
)

add_library(crashomon_client_static STATIC
  crashomon.cpp
  exception_capture.cpp
)

# exception_capture.cpp uses try/catch to inspect in-flight exceptions, so it
# must be compiled with exception support even though all other sources use
# -fno-exceptions.  The per-file -fexceptions flag overrides the global flag
# (last flag wins for GCC/Clang).
set_source_files_properties(exception_capture.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
set_target_properties(crashomon_client_static PROPERTIES OUTPUT_NAME crashomon)

foreach(target crashomon_client crashomon_client_static)
  target_link_libraries(${target}
    PRIVATE
      crashomon_warnings
      crashpad_client
      ${CMAKE_DL_LIBS}
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
  set_target_properties(${target} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}"
  )
endforeach()

set_target_properties(crashomon_client PROPERTIES OUTPUT_NAME crashomon)
