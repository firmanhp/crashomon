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

#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace crashomon {
namespace {

int DoInit(const ResolvedConfig& cfg) {
  int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock < 0) return -1;

  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, cfg.socket_path.c_str(), sizeof(addr.sun_path) - 1);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return -1;  // watcherd not running — crash monitoring silently disabled
  }

  int one = 1;
  setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

  // pid=-1: SetHandlerSocket discovers watcherd PID via SCM_CREDENTIALS,
  // calls prctl(PR_SET_PTRACER, watcherd_pid), and installs crash signal handlers.
  static crashpad::CrashpadClient client;
  return client.SetHandlerSocket(base::ScopedFD(sock), /*pid=*/-1) ? 0 : -1;
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
