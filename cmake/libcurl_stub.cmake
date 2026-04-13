# cmake/libcurl_stub.cmake — Minimal libcurl stub for Crashpad compilation
#
# Crashpad's util/net/http_transport_libcurl.cc unconditionally compiles on
# Linux and includes <curl/curl.h>. With no upload URL configured, this code
# is never called at runtime — Crashpad loads libcurl via dlopen only when
# the HTTP transport is actually used.
#
# This module:
#   1. Copies cmake/curl_stub/curl.h into the build tree as curl/curl.h so the
#      #include <curl/curl.h> in Crashpad's source resolves at compile time.
#   2. Defines a CURL::libcurl IMPORTED INTERFACE target (not ALIAS) so it is
#      valid in crashpad_export sets. ALIAS targets of non-IMPORTED builds
#      cause export-set validation failures; IMPORTED targets are self-describing.
#   3. Sets the FindCURL cache variables so any downstream find_package(CURL)
#      call short-circuits and uses this stub.

set(_curl_stub_include_dir "${CMAKE_BINARY_DIR}/curl_stub")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/curl_stub/curl.h"
  "${_curl_stub_include_dir}/curl/curl.h"
  COPYONLY
)

if(NOT TARGET CURL::libcurl)
  add_library(CURL::libcurl IMPORTED INTERFACE GLOBAL)
  set_target_properties(CURL::libcurl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_curl_stub_include_dir}"
  )
  set(CURL_FOUND          TRUE                        CACHE BOOL   "" FORCE)
  set(CURL_INCLUDE_DIR    "${_curl_stub_include_dir}" CACHE PATH   "" FORCE)
  set(CURL_LIBRARY        "CURL::libcurl"             CACHE STRING "" FORCE)
  set(CURL_LIBRARIES      "CURL::libcurl"             CACHE STRING "" FORCE)
  set(CURL_VERSION_STRING "7.0.0"                     CACHE STRING "" FORCE)
endif()
