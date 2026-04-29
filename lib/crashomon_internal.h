// lib/crashomon_internal.h — Internal configuration resolution logic.
//
// Not part of the public API.  Exposed as inline functions so unit tests can
// include this header and exercise the logic without linking sentry-native.
//
// Only crashomon.cpp and unit tests should include this header.

#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>

namespace crashomon {

constexpr std::string_view kDefaultDbPath = "/var/crashomon";
constexpr std::string_view kDefaultSocketPath = "/run/crashomon/handler.sock";

// Returns the value of environment variable `name`, or std::nullopt if unset.
// The returned string_view is valid only as long as the environment is not
// modified (i.e. no subsequent setenv / unsetenv / putenv calls).
[[nodiscard]] inline std::optional<std::string_view> GetEnv(const char* name) noexcept {
  const char* val = std::getenv(name);
  if (val == nullptr) {
    return std::nullopt;
  }
  return std::string_view{val};
}

// Resolved configuration: paths chosen after applying precedence rules.
struct ResolvedConfig {
  std::string db_path;
  std::string socket_path;
};

// Resolve db_path and socket_path by applying, in order:
//   1. Environment variable (CRASHOMON_DB_PATH / CRASHOMON_SOCKET_PATH)
//   2. Compiled-in default (kDefaultDbPath / kDefaultSocketPath)
[[nodiscard]] inline ResolvedConfig Resolve() {
  ResolvedConfig resolved;

  if (auto env = GetEnv("CRASHOMON_DB_PATH")) {
    resolved.db_path = std::string{*env};
  } else {
    resolved.db_path = std::string{kDefaultDbPath};
  }

  if (auto env = GetEnv("CRASHOMON_SOCKET_PATH")) {
    resolved.socket_path = std::string{*env};
  } else {
    resolved.socket_path = std::string{kDefaultSocketPath};
  }

  return resolved;
}

// Connect to watcherd and configure the Crashpad handler.  Exposed here so
// tests can invoke it directly with a custom ResolvedConfig.
int DoInit(const ResolvedConfig& cfg);

// Formats an assert() failure annotation and writes it to abort_message using
// a 512-byte stack buffer (no heap allocation).  Safe to call before DoInit()
// — crashomon_set_abort_message is a no-op when the annotations pointer is null.
void WriteAssertAnnotation(const char* assertion, const char* file, unsigned int line,
                           const char* func) noexcept;

// Captures e.what() from the current in-flight exception into buf.
// No-op when there is no active exception or the exception is not a
// std::exception subclass.  Implemented in exception_capture.cpp, compiled
// with -fexceptions, so try/catch is available there.
void CaptureCurrentExceptionMessage(char* buf, size_t buf_size) noexcept;

// Writes terminate_type (demangled exception class name) and abort_message.
// Pass abi::__cxa_current_exception_type() for exc_type; pass nullptr when
// there is no active exception.  what_msg (if non-null and non-empty) is used
// as the abort_message; otherwise "unhandled C++ exception" is the fallback.
void WriteTerminateAnnotation(const std::type_info* exc_type,
                              const char* what_msg = nullptr) noexcept;

}  // namespace crashomon
