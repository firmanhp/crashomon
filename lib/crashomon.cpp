// lib/crashomon.cpp — C++17 implementation of the crashomon client library.
//
// The public interface (crashomon.h) is a pure C API with no C++ dependencies.
// All C++ internals are confined to this translation unit and crashomon_internal.h.
//
// Integration modes:
//   LD_PRELOAD   — constructor/destructor handle init and shutdown automatically.
//   Explicit     — call crashomon_init() / crashomon_shutdown() at program start/end.

#include "crashomon.h"
#include "crashomon_internal.h"

#include <sentry.h>

#include <string_view>

namespace crashomon {
namespace {

// Dummy DSN: sentry-native requires a non-empty DSN but we disable all uploads.
constexpr std::string_view kDummyDsn =
    "https://00000000000000000000000000000000@localhost/0";

int DoInit(const ResolvedConfig& cfg) {
  sentry_options_t* options = sentry_options_new();
  if (!options) return -1;

  sentry_options_set_dsn(options, kDummyDsn.data());
  sentry_options_set_database_path(options, cfg.db_path.c_str());
  sentry_options_set_handler_path(options, cfg.handler_path.c_str());

  // Disable all telemetry uploads — crash data never leaves the device.
  sentry_options_set_auto_session_tracking(options, 0);
  sentry_options_set_traces_sample_rate(options, 0.0);

  return sentry_init(options);
}

}  // namespace
}  // namespace crashomon

// ── LD_PRELOAD constructor / destructor ──────────────────────────────────────
// GCC/Clang constructor/destructor attributes for shared library lifecycle.
// These fire automatically when the library is loaded/unloaded — no code
// changes are required in the monitored process.

__attribute__((constructor)) static void crashomon_auto_init() {
  crashomon::DoInit(crashomon::Resolve(nullptr));
}

__attribute__((destructor)) static void crashomon_auto_shutdown() {
  sentry_close();
}

// ── Public C API ─────────────────────────────────────────────────────────────

int crashomon_init(const CrashomonConfig* config) {
  return crashomon::DoInit(crashomon::Resolve(config));
}

void crashomon_shutdown() { sentry_close(); }

void crashomon_set_tag(const char* key, const char* value) {
  if (key && value) sentry_set_tag(key, value);
}

void crashomon_add_breadcrumb(const char* message) {
  if (!message) return;
  sentry_value_t crumb = sentry_value_new_breadcrumb("default", message);
  sentry_add_breadcrumb(crumb);
}

void crashomon_set_abort_message(const char* message) {
  if (message) sentry_set_tag("abort_message", message);
}
