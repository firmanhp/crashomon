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

#include "crashomon.h"

namespace crashomon {

constexpr std::string_view kDefaultDbPath =
    "/var/crashomon";
constexpr std::string_view kDefaultSocketPath =
    "/run/crashomon/handler.sock";

// Returns the value of environment variable `name`, or std::nullopt if unset.
// The returned string_view is valid only as long as the environment is not
// modified (i.e. no subsequent setenv / unsetenv / putenv calls).
[[nodiscard]] inline std::optional<std::string_view> GetEnv(
    const char* name) noexcept {
  const char* val = std::getenv(name);
  if (!val) return std::nullopt;
  return std::string_view{val};
}

// Resolved configuration: paths chosen after applying precedence rules.
struct ResolvedConfig {
  std::string db_path;
  std::string socket_path;
};

// Resolve db_path and socket_path by applying, in order:
//   1. Explicit CrashomonConfig field (if config != nullptr and field != nullptr)
//   2. Environment variable (CRASHOMON_DB_PATH / CRASHOMON_SOCKET_PATH)
//   3. Compiled-in default (kDefaultDbPath / kDefaultSocketPath)
[[nodiscard]] inline ResolvedConfig Resolve(const CrashomonConfig* config) {
  ResolvedConfig resolved;

  if (config && config->db_path) {
    resolved.db_path = config->db_path;
  } else if (auto env = GetEnv("CRASHOMON_DB_PATH")) {
    resolved.db_path = std::string{*env};
  } else {
    resolved.db_path = std::string{kDefaultDbPath};
  }

  if (config && config->socket_path) {
    resolved.socket_path = config->socket_path;
  } else if (auto env = GetEnv("CRASHOMON_SOCKET_PATH")) {
    resolved.socket_path = std::string{*env};
  } else {
    resolved.socket_path = std::string{kDefaultSocketPath};
  }

  return resolved;
}

}  // namespace crashomon
