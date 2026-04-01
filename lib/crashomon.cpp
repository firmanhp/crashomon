// lib/crashomon.cpp — C++17 implementation of the crashomon client library.
//
// The public interface (crashomon.h) is a pure C API with no C++ dependencies.
// All C++ internals are confined to this translation unit and crashomon_internal.h.
//
// Integration modes:
//   LD_PRELOAD   — constructor/destructor handle init and shutdown automatically.
//   Explicit     — call crashomon_init() / crashomon_shutdown() at program start/end.

#include "crashomon.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "base/files/scoped_file.h"
#include "client/crashpad_client.h"
#include "crashomon_internal.h"

namespace crashomon {
namespace {

// Receive the shared Crashpad socket fd and handler PID from watcherd via
// SCM_RIGHTS.  Returns the received fd on success, or -1 on failure.
// On success, *out_pid is set to the handler PID.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int ReceiveSharedSocket(int conn_fd, pid_t* out_pid) {
  pid_t pid = 0;
  struct iovec iov {};
  iov.iov_base = &pid;
  iov.iov_len = sizeof(pid);

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays)
  char cmsg_buf[CMSG_SPACE(sizeof(int))]{};

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf;
  msg.msg_controllen = sizeof(cmsg_buf);

  const ssize_t n = recvmsg(conn_fd, &msg, /*flags=*/0);
  if (n < static_cast<ssize_t>(sizeof(pid))) {
    return -1;
  }

  // Extract the file descriptor from SCM_RIGHTS ancillary data.
  // CMSG macros use pointer casts internally; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    return -1;
  }

  int shared_fd = -1;
  std::memcpy(&shared_fd, CMSG_DATA(cmsg), sizeof(int));
  *out_pid = pid;
  return shared_fd;
}

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int DoInit(const ResolvedConfig& cfg) {
  const int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    return -1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  // sun_path is a POSIX
  // fixed-size C array; strncpy is the prescribed way to fill it.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, cfg.socket_path.c_str(), sizeof(addr.sun_path) - 1);

  // POSIX bind/connect require
  // casting sockaddr_un* to sockaddr*; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(sock);
    return -1;  // watcherd not running — crash monitoring silently disabled
  }

  // Receive the shared Crashpad socket fd and handler PID from watcherd.
  // The daemon sends them via SCM_RIGHTS over the registration connection.
  pid_t handler_pid = 0;
  const int shared_fd = ReceiveSharedSocket(sock, &handler_pid);
  close(sock);  // registration exchange done
  if (shared_fd < 0) {
    return -1;
  }

  // Explicit handler_pid (not -1) is required: with a shared socket, the
  // kTypeCheckCredentials reply could be consumed by the wrong process.
  // SO_PASSCRED is already set on the socket by the daemon before sharing.
  static crashpad::CrashpadClient client;
  return client.SetHandlerSocket(base::ScopedFD(shared_fd), handler_pid) ? 0 : -1;
}

// ── LD_PRELOAD constructor / destructor ─────────────────────────────────────
// GCC/Clang constructor/destructor attributes for shared library lifecycle.
// These fire automatically when the library is loaded/unloaded — no code
// changes are required in the monitored process.

__attribute__((constructor)) void AutoInit() { DoInit(Resolve(nullptr)); }

__attribute__((destructor)) void AutoShutdown() {
  // Crashpad handler runs as an independent process; no shutdown needed.
}

}  // namespace
}  // namespace crashomon

// ── Public C API ─────────────────────────────────────────────────────────────
// C-compatible names — GlobalFunctionCase: lower_case in .clang-tidy covers these.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
int crashomon_init(const CrashomonConfig* config) {
  return crashomon::DoInit(crashomon::Resolve(config));
}

void crashomon_shutdown() {
  // Crashpad handler runs as an independent process; no shutdown needed.
}

// key/value is the standard C tag API
// pattern; parameter names make intent unambiguous.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
