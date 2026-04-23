// daemon/log.h — spdlog initialisation for crashomon-watcherd
//
// Call InitLogging() once at startup, before any spdlog calls.
//
// Systemd detection: if the JOURNAL_STREAM environment variable is set,
// watcherd is running under systemd and journald is capturing stderr.
// Journald already records timestamps for every log entry, so printing a
// second timestamp from spdlog is pure noise.  We suppress it in that case.
// JOURNAL_STREAM has been set by systemd for all service units since v231.

#pragma once

#include <cstdlib>

#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"

namespace crashomon {

inline void InitLogging() {
  auto logger = std::make_shared<spdlog::logger>("watcherd",
                                                 std::make_shared<spdlog::sinks::stderr_sink_mt>());
  spdlog::set_default_logger(logger);

  if (std::getenv("JOURNAL_STREAM") != nullptr) {
    // Under systemd: journald timestamps every entry — omit ours.
    spdlog::set_pattern("[%l] %v");
  } else {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
  }
  spdlog::set_level(spdlog::level::info);
}

}  // namespace crashomon
