// daemon/crash_socket.h — Unix domain socket for crash handler registration
//
// CreateListenSocket creates a SOCK_SEQPACKET Unix socket that client processes
// connect to to receive a shared Crashpad socket fd.
// AcceptAndShareSocket accepts one such connection and passes the shared fd via
// SCM_RIGHTS.

#pragma once

#include <sys/types.h>

#include <string>

namespace crashomon {

// Create, bind, and listen on a SOCK_SEQPACKET Unix domain socket at
// socket_path.  Refuses to start if a live daemon already owns the path.
// Returns the listening fd on success, or -1 on failure.
int CreateListenSocket(const std::string& socket_path);

// Accept one registration connection on listen_fd and send shared_client_fd +
// handler_pid to the registering process via SCM_RIGHTS.  Non-blocking: a
// single sendmsg call.
// listen_fd (incoming connections) and shared_client_fd (pre-created Crashpad
// socket) are both int but serve different roles; pid_t distinguishes the third.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void AcceptAndShareSocket(int listen_fd, int shared_client_fd, pid_t handler_pid);

}  // namespace crashomon
