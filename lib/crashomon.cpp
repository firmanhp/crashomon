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

#include "client/crashpad_client.h"

#include <map>
#include <string>
#include <vector>

namespace crashomon {
namespace {

int DoInit(const ResolvedConfig& cfg) {
  base::FilePath handler(cfg.handler_path);
  base::FilePath database(cfg.db_path);

  std::map<std::string, std::string> annotations;
  std::vector<std::string> arguments;

  // url="" disables all uploads — crash data never leaves the device.
  static crashpad::CrashpadClient client;
  bool started = client.StartHandler(
      handler,
      database,
      /*metrics_dir=*/database,
      /*url=*/"",
      /*http_proxy=*/"",
      annotations,
      arguments,
      /*restartable=*/true,
      /*asynchronous_start=*/false);

  return started ? 0 : -1;
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
  // Crashpad handler runs as an independent process; no shutdown needed.
}

// ── Public C API ─────────────────────────────────────────────────────────────

int crashomon_init(const CrashomonConfig* config) {
  return crashomon::DoInit(crashomon::Resolve(config));
}

void crashomon_shutdown() {
  // Crashpad handler runs as an independent process; no shutdown needed.
}

void crashomon_set_tag(const char* key, const char* value) {
  // TODO: Implement with crashpad::Annotation in a follow-up.
  (void)key;
  (void)value;
}

void crashomon_add_breadcrumb(const char* message) {
  // Crashpad has no breadcrumb concept. No-op.
  (void)message;
}

void crashomon_set_abort_message(const char* message) {
  // TODO: Implement with crashpad::Annotation in a follow-up.
  (void)message;
}
