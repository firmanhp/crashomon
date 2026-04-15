// daemon/crash_socket.cpp — Unix domain socket for crash handler registration

#include "daemon/crash_socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string>

#include "base/files/scoped_file.h"
#include "spdlog/spdlog.h"

namespace {

constexpr mode_t kSocketPermissions = 0666;
constexpr int kListenBacklog = 128;

}  // namespace

namespace crashomon {

int CreateListenSocket(const std::string& socket_path) {
  base::ScopedFD sock_fd_scoped(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  if (!sock_fd_scoped.is_valid()) {
    perror("socket");
    return -1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  // Fill sun_path; explicitly null-terminate to handle paths that exactly fill the buffer.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

  // Probe: if a live daemon already owns the socket, refuse to steal it.
  {
    const base::ScopedFD probe(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (probe.is_valid()) {
      // POSIX connect requires casting sockaddr_un* to sockaddr*; no standard-compliant
      // alternative. NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
      if (connect(probe.get(), reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) ==
          0) {
        spdlog::error("crashomon-watcherd: socket {} is held by a live daemon — refusing to start",
                      socket_path);
        return -1;
      }
      // ENOENT / ECONNREFUSED / ENOTSOCK → stale socket, safe to unlink.
    }
  }

  // Remove stale socket from a previous run.
  unlink(socket_path.c_str());

  // POSIX bind/connect require casting sockaddr_un* to sockaddr*; no standard-compliant
  // alternative. NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (bind(sock_fd_scoped.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("bind");
    return -1;
  }

  // Allow all users to connect (monitored processes may run as different users).
  chmod(socket_path.c_str(), kSocketPermissions);

  if (listen(sock_fd_scoped.get(), /*backlog=*/kListenBacklog) != 0) {
    perror("listen");
    return -1;
  }

  return sock_fd_scoped.release();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void AcceptAndShareSocket(int listen_fd, int shared_client_fd, pid_t handler_pid) {
  const int conn_fd = accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
  if (conn_fd < 0) {
    if (errno != EINTR && errno != EAGAIN) {
      perror("accept4");
    }
    return;
  }

  // iovec payload: handler PID so the client can call prctl(PR_SET_PTRACER).
  struct iovec iov {};
  iov.iov_base = &handler_pid;
  iov.iov_len = sizeof(handler_pid);

  // cmsg: pass shared_client_fd via SCM_RIGHTS (kernel dups the fd for the recipient process).
  std::array<char, CMSG_SPACE(sizeof(int))> cmsg_buf{};

  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf.data();
  msg.msg_controllen = cmsg_buf.size();

  // CMSG macros use pointer casts internally; no standard-compliant alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(cmsg), &shared_client_fd, sizeof(int));

  // MSG_NOSIGNAL prevents SIGPIPE if the client disconnected before receiving
  // (e.g. crashed immediately after connect).
  if (sendmsg(conn_fd, &msg, MSG_NOSIGNAL) < 0) {
    if (errno != EPIPE) {
      perror("sendmsg(SCM_RIGHTS)");
    }
  }

  close(conn_fd);
}

}  // namespace crashomon
