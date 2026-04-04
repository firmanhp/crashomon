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

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "base/files/scoped_file.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "crashomon_internal.h"

namespace crashomon {
namespace {

// Receive the shared Crashpad socket fd and handler PID from watcherd via
// SCM_RIGHTS.  Returns the received fd on success, or -1 on failure.
// On success, *out_pid is set to the handler PID.
int ReceiveSharedSocket(int conn_fd, pid_t* out_pid) {
  pid_t pid = 0;
  struct iovec iov {};
  iov.iov_base = &pid;
  iov.iov_len = sizeof(pid);

  std::array<char, CMSG_SPACE(sizeof(int))> cmsg_buf{};

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf.data();
  msg.msg_controllen = cmsg_buf.size();

  const ssize_t recv_len = recvmsg(conn_fd, &msg, /*flags=*/0);
  if (recv_len < static_cast<ssize_t>(sizeof(pid))) {
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

int DoInit(const ResolvedConfig& cfg) {
  const int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock < 0) {
    return -1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  // Fill sun_path; explicitly null-terminate to handle paths that exactly fill the buffer.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, cfg.socket_path.c_str(), sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

  // POSIX bind/connect require
  // casting sockaddr_un* to sockaddr*; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    // Warn via stderr — captured by journald when running under systemd,
    // and visible in /proc/<pid>/fd/2 for interactive processes.
    std::fputs("crashomon: could not connect to watcherd at ", stderr);
    std::fputs(cfg.socket_path.c_str(), stderr);
    std::fputs(": ", stderr);
    std::fputs(strerror(errno), stderr);
    std::fputc('\n', stderr);
    close(sock);
    return -1;
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
  if (!client.SetHandlerSocket(base::ScopedFD(shared_fd), handler_pid)) {
    return -1;
  }

  // Attach our SimpleStringDictionary to CrashpadInfo so the handler picks
  // up tags written via crashomon_set_tag().
  static crashpad::SimpleStringDictionary annotations;
  crashpad::CrashpadInfo::GetCrashpadInfo()->set_simple_annotations(&annotations);
  return 0;
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
int crashomon_init(const CrashomonConfig* config) {
  return crashomon::DoInit(crashomon::Resolve(config));
}

void crashomon_shutdown() {
  // Crashpad handler runs as an independent process; no shutdown needed.
}

void crashomon_set_abort_message(const char* message) {
  crashomon_set_tag("abort_message", message);
}

// key/value is the standard C tag API
// pattern; parameter names make intent unambiguous.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void crashomon_set_tag(const char* key, const char* value) {
  auto* info = crashpad::CrashpadInfo::GetCrashpadInfo();
  auto* annotations = info->simple_annotations();
  if (annotations == nullptr || key == nullptr || value == nullptr) {
    return;
  }
  annotations->SetKeyValue(key, value);
}
