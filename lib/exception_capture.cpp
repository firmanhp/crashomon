// lib/exception_capture.cpp — captures e.what() from an in-flight exception.
//
// Compiled with -fexceptions (per set_source_files_properties in CMakeLists.txt)
// so that try/catch is available.  All other crashomon sources use -fno-exceptions.

#include "crashomon_internal.h"

#include <cstdio>
#include <cstddef>
#include <exception>

namespace crashomon {

void CaptureCurrentExceptionMessage(char* buf, size_t buf_size) noexcept {
  if (buf == nullptr || buf_size == 0) {
    return;
  }
  *buf = '\0';
  if (std::current_exception() == nullptr) {
    return;
  }
  try {
    std::rethrow_exception(std::current_exception());
  } catch (const std::exception& e) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::snprintf(buf, buf_size, "%s", e.what());
  } catch (...) {
    // Non-std::exception: no message to capture.
  }
}

}  // namespace crashomon
