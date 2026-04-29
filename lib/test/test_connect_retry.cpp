// test/test_connect_retry.cpp — unit tests for the 3-second connection retry
// behavior in DoInit() (lib/crashomon.cpp).
//
// Each test calls crashomon::DoInit() directly against a temporary
// AF_UNIX/SOCK_SEQPACKET socket in /tmp and checks timing, return value, and
// stderr output.  CRASHOMON_TESTING_SKIP_AUTOINIT suppresses the library
// constructor so startup doesn't burn 3s against an absent watcherd.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/crashomon_internal.h"

namespace {

// RAII helper that redirects STDERR_FILENO to a pipe on construction.
// Drain() restores the original fd and returns everything written to it.
class StderrCapture {
 public:
  StderrCapture() : saved_stderr_(dup(STDERR_FILENO)) {
    const int ret = pipe2(pipe_.data(), O_CLOEXEC);
    (void)ret;  // failure leaves pipe_[0/1] = -1; Drain() will skip them
    dup2(pipe_[1], STDERR_FILENO);
    close(pipe_[1]);
    pipe_[1] = -1;
  }

  ~StderrCapture() {
    if (saved_stderr_ >= 0) {
      dup2(saved_stderr_, STDERR_FILENO);
      close(saved_stderr_);
    }
    if (pipe_[0] >= 0) {
      close(pipe_[0]);
    }
  }

  // Restores stderr and drains all buffered output from the pipe.
  // May only be called once.
  std::string Drain() {
    fflush(stderr);
    dup2(saved_stderr_, STDERR_FILENO);
    close(saved_stderr_);
    saved_stderr_ = -1;

    constexpr std::size_t read_buf_len = 256;
    std::string result;
    std::array<char, read_buf_len> buf{};
    ssize_t num_read = 0;
    while ((num_read = read(pipe_[0], buf.data(), buf.size())) > 0) {
      result.append(buf.data(), static_cast<size_t>(num_read));
    }
    close(pipe_[0]);
    pipe_[0] = -1;
    return result;
  }

  StderrCapture(const StderrCapture&) = delete;
  StderrCapture& operator=(const StderrCapture&) = delete;
  StderrCapture(StderrCapture&&) = delete;
  StderrCapture& operator=(StderrCapture&&) = delete;

 private:
  std::array<int, 2> pipe_ = {-1, -1};
  int saved_stderr_ = -1;
};

// Returns a per-process unique socket path for the given tag, so parallel test
// binaries do not collide.
std::string SocketPath(const char* tag) {
  return "/tmp/crashomon_retry_" + std::to_string(::getpid()) + "_" + tag + ".sock";
}

// Creates a listening AF_UNIX SOCK_SEQPACKET socket at path.
// Returns the server fd, or -1 on failure.
int MakeListeningSocket(const std::string& path) {
  ::unlink(path.c_str());
  const int sock_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (sock_fd < 0) {
    return -1;
  }
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (::bind(sock_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(sock_fd, 1) != 0) {
    ::close(sock_fd);
    ::unlink(path.c_str());
    return -1;
  }
  return sock_fd;
}

// ── ConnectRetry tests ────────────────────────────────────────────────────────

// When the socket never appears, DoInit() should retry until the deadline,
// print a one-time "waiting" notice on the first retry, then print a "not
// available after Ns" error and return non-zero.  The process continues —
// no abort or crash.
TEST(ConnectRetryTest, TimeoutPrintsMessagesAndReturnsFailure) {
  const std::string path = SocketPath("timeout");
  ::unlink(path.c_str());

  StderrCapture cap;
  const auto start = std::chrono::steady_clock::now();
  const int result = crashomon::DoInit(crashomon::ResolvedConfig{"", path, 1});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const std::string output = cap.Drain();

  EXPECT_NE(result, 0);
  EXPECT_GE(elapsed, std::chrono::seconds(1));
  EXPECT_LT(elapsed, std::chrono::seconds(3));
  EXPECT_NE(output.find("waiting for watcherd"), std::string::npos)
      << "expected 'waiting' notice in stderr; got:\n"
      << output;
  EXPECT_NE(output.find("not available after 1s"), std::string::npos)
      << "expected timeout error in stderr; got:\n"
      << output;
}

// When the socket becomes available partway through the retry window, DoInit()
// should connect before the 3s deadline.  The one-time "waiting" notice should
// appear (a retry did occur), but the "not available" error should not.
TEST(ConnectRetryTest, SocketAppearsBeforeTimeoutConnectsEarly) {
  const std::string path = SocketPath("late");
  ::unlink(path.c_str());

  // Server thread: wait 300ms, create the socket, accept one connection, and
  // close it immediately.  Closing the accepted fd makes ReceiveSharedSocket
  // fail on the client side (recvmsg returns EOF), which is expected here —
  // we are only testing the retry behavior, not the full handshake.
  constexpr int delay_ms = 300;
  std::atomic<int> server_fd{-1};
  std::thread server_thread([&path, &server_fd, delay_ms] {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    const int srv_fd = MakeListeningSocket(path);
    server_fd.store(srv_fd);
    if (srv_fd < 0) {
      return;
    }
    const int client_fd = ::accept(srv_fd, nullptr, nullptr);
    if (client_fd >= 0) {
      ::close(client_fd);
    }
  });

  StderrCapture cap;
  const auto start = std::chrono::steady_clock::now();
  const int result = crashomon::DoInit(crashomon::ResolvedConfig{"", path});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const std::string output = cap.Drain();

  server_thread.join();
  if (const int sfd = server_fd.load(); sfd >= 0) {
    ::close(sfd);
    ::unlink(path.c_str());
  }

  EXPECT_NE(result, 0);  // ReceiveSharedSocket failed — expected.
  EXPECT_GE(elapsed, std::chrono::milliseconds(200))
      << "connected faster than the 300ms server delay";
  EXPECT_LT(elapsed, std::chrono::seconds(3)) << "did not connect within the 3s window";
  EXPECT_NE(output.find("waiting for watcherd"), std::string::npos)
      << "expected 'waiting' notice (retry occurred); got:\n"
      << output;
  EXPECT_EQ(output.find("not available after 3s"), std::string::npos)
      << "unexpected timeout error; got:\n"
      << output;
}

// When the socket is already listening before DoInit() is called, the first
// connect() succeeds — no retry, so no "waiting" message should appear.
TEST(ConnectRetryTest, ImmediateConnectPrintsNoWaitingMessage) {
  const std::string path = SocketPath("immediate");
  ::unlink(path.c_str());

  const int srv_fd = MakeListeningSocket(path);
  ASSERT_GE(srv_fd, 0) << "failed to create listening socket at " << path;

  // Accept in a background thread so the server is ready when the client
  // connects.  Closing immediately makes ReceiveSharedSocket fail on the
  // client — expected, we are not testing the full handshake.
  std::thread server_thread([srv_fd] {
    const int client_fd = ::accept(srv_fd, nullptr, nullptr);
    if (client_fd >= 0) {
      ::close(client_fd);
    }
  });

  StderrCapture cap;
  const auto start = std::chrono::steady_clock::now();
  const int result = crashomon::DoInit(crashomon::ResolvedConfig{"", path});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const std::string output = cap.Drain();

  server_thread.join();
  ::close(srv_fd);
  ::unlink(path.c_str());

  EXPECT_NE(result, 0);  // ReceiveSharedSocket failed — expected.
  EXPECT_LT(elapsed, std::chrono::seconds(1));
  EXPECT_EQ(output.find("waiting for watcherd"), std::string::npos)
      << "unexpected 'waiting' message on immediate connect; got:\n"
      << output;
  EXPECT_EQ(output.find("not available after 3s"), std::string::npos)
      << "unexpected timeout error on immediate connect; got:\n"
      << output;
}

// With connect_timeout_sec=0 DoInit() tries once and returns immediately on
// ENOENT/ECONNREFUSED — no 3-second retry, no "waiting" message.
TEST(ConnectRetryTest, ZeroTimeoutReturnsImmediately) {
  const std::string path = SocketPath("zero_timeout");
  ::unlink(path.c_str());

  StderrCapture cap;
  const auto start = std::chrono::steady_clock::now();
  const int result = crashomon::DoInit(crashomon::ResolvedConfig{"", path, 0});
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const std::string output = cap.Drain();

  EXPECT_NE(result, 0);
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  EXPECT_EQ(output.find("waiting for watcherd"), std::string::npos)
      << "unexpected 'waiting' message with zero timeout; got:\n"
      << output;
  EXPECT_NE(output.find("not available after 0s"), std::string::npos)
      << "expected 'not available after 0s' in stderr; got:\n"
      << output;
}

}  // namespace
