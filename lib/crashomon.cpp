// lib/crashomon.cpp — C++20 implementation of the crashomon client library.
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

#include <dlfcn.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <string>

#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "client/crashpad_client.h"
#include "client/crashpad_info.h"
#include "client/simple_string_dictionary.h"
#include "crashomon_internal.h"

// Forward declaration so DoInit can verify interposition at runtime.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, readability-identifier-naming)
extern "C" [[noreturn]] void __assert_fail(const char* assertion, const char* file,
                                            unsigned int line, const char* func) noexcept;

namespace crashomon {
namespace {

// Swallow the known-harmless `prctl` WARNING emitted by Crashpad's
// crashpad_client_linux.cc when `PR_SET_PTRACER` fails with EINVAL on kernels
// without the Yama LSM. Without Yama, the restriction PR_SET_PTRACER relaxes
// does not exist, so the handler can still ptrace the client fine. All other
// Crashpad log messages pass through unchanged.
bool CrashpadLogFilter(logging::LogSeverity severity, const char* file, int /*line*/,
                       size_t message_start, const std::string& str) {
  // Suppress harmless PR_SET_PTRACER warning from Crashpad under LD_PRELOAD.
  // Return true = suppress, false = let mini_chromium print normally.
  return severity == logging::LOG_WARNING && file != nullptr &&
         std::strstr(file, "crashpad_client_linux.cc") != nullptr &&
         str.find("prctl", message_start) != std::string::npos;
}

void InstallCrashpadLogFilterOnce() {
  static std::once_flag once;
  std::call_once(once, [] { logging::SetLogMessageHandler(&CrashpadLogFilter); });
}

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
  if (cmsg == nullptr || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    return -1;
  }

  int shared_fd = -1;
  std::memcpy(&shared_fd, CMSG_DATA(cmsg), sizeof(int));
  *out_pid = pid;
  return shared_fd;
}

int DoInit(const ResolvedConfig& cfg) {
  // Silence Crashpad's harmless PR_SET_PTRACER EINVAL warning (from
  // crashpad_client_linux.cc:393 on SetHandlerSocket and :448 on every fork)
  // before any Crashpad code runs.
  InstallCrashpadLogFilterOnce();

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

  // Retry connecting for up to 3 seconds to tolerate watcherd still starting up.
  // ENOENT  — socket file not yet created.
  // ECONNREFUSED — socket exists but watcherd not yet listening.
  static constexpr int kConnectTimeoutSec = 3;
  static constexpr long kRetryIntervalNs = 100'000'000L;  // 100 ms

  struct timespec deadline {};
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += kConnectTimeoutSec;

  bool connected = false;
  bool waiting_printed = false;
  int connect_errno = 0;
  while (true) {
    // POSIX bind/connect require casting sockaddr_un* to sockaddr*; no standard-compliant
    // alternative.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    connect_errno = errno;
    if (connect_errno != ENOENT && connect_errno != ECONNREFUSED) {
      break;  // Non-transient error — do not retry.
    }
    struct timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
      break;  // Deadline reached.
    }
    if (!waiting_printed) {
      std::fputs("crashomon: waiting for watcherd at ", stderr);
      std::fputs(cfg.socket_path.c_str(), stderr);
      std::fputs(" (up to 3s)...\n", stderr);
      waiting_printed = true;
    }
    const struct timespec interval {
      0, kRetryIntervalNs
    };
    nanosleep(&interval, nullptr);
  }

  if (!connected) {
    // Warn via stderr — captured by journald when running under systemd,
    // and visible in /proc/<pid>/fd/2 for interactive processes.
    std::fputs("crashomon: could not connect to watcherd at ", stderr);
    std::fputs(cfg.socket_path.c_str(), stderr);
    if (connect_errno == ENOENT || connect_errno == ECONNREFUSED) {
      std::fputs(": not available after 3s, running without crash monitoring\n", stderr);
    } else {
      std::fputs(": ", stderr);
      std::fputs(strerror(connect_errno), stderr);
      std::fputc('\n', stderr);
    }
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

  std::set_terminate([]() noexcept {
    crashomon::WriteTerminateAnnotation(abi::__cxa_current_exception_type());
    std::abort();
  });

  // Verify that the dynamic linker resolved __assert_fail to our definition.
  // Fails silently when libcrashomon.so is linked explicitly (not via LD_PRELOAD)
  // and libc appeared first in the DSO load order.
  {
    void* const resolved = dlsym(RTLD_DEFAULT, "__assert_fail");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    void* const ours = reinterpret_cast<void*>(&__assert_fail);
    if (resolved != nullptr && resolved != ours) {
      std::fputs("crashomon: warning: __assert_fail interposition failed — "
                 "assert() failures will not include message text. "
                 "Use LD_PRELOAD or link libcrashomon.a instead.\n",
                 stderr);
    }
  }

  return 0;
}

// ── LD_PRELOAD constructor / destructor ─────────────────────────────────────
// GCC/Clang constructor/destructor attributes for shared library lifecycle.
// These fire automatically when the library is loaded/unloaded — no code
// changes are required in the monitored process.

// CRASHOMON_TESTING_SKIP_AUTOINIT is defined by the client test binary so the
// constructor does not burn 3 s retrying against an absent watcherd at startup.
// Tests that exercise the retry logic call crashomon_init() directly.
#ifndef CRASHOMON_TESTING_SKIP_AUTOINIT
__attribute__((constructor)) void AutoInit() { DoInit(Resolve(nullptr)); }
#endif

__attribute__((destructor)) void AutoShutdown() {
  // Crashpad handler runs as an independent process; no shutdown needed.
}

}  // namespace

void WriteAssertAnnotation(const char* assertion, const char* file,
                            unsigned int line, const char* func) noexcept {
  const std::string msg =
      std::format("assertion failed: '{}' ({}:{}, {})",
                  assertion != nullptr ? assertion : "?",
                  file != nullptr ? file : "?",
                  line,
                  func != nullptr ? func : "?");
  crashomon_set_abort_message(msg.c_str());
}

void WriteTerminateAnnotation(const std::type_info* exc_type) noexcept {
  if (exc_type != nullptr) {
    int status = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    auto demangled = std::unique_ptr<char, decltype(&std::free)>(
        abi::__cxa_demangle(exc_type->name(), nullptr, nullptr, &status),
        std::free);
    const char* type_name =
        (status == 0 && demangled != nullptr) ? demangled.get() : exc_type->name();
    crashomon_set_tag("terminate_type", type_name);
    crashomon_set_abort_message("unhandled C++ exception");
  } else {
    crashomon_set_abort_message("terminate called without active exception");
  }
}

}  // namespace crashomon

// Override glibc's __assert_fail so assert() failures are captured as annotations
// before SIGABRT fires. The dynamic linker resolves this definition first when
// libcrashomon.so is LD_PRELOAD'd.
// NOLINTNEXTLINE(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, readability-identifier-naming)
extern "C" [[noreturn]] void __assert_fail(const char* assertion, const char* file,
                                            unsigned int line,
                                            const char* func) noexcept {
  crashomon::WriteAssertAnnotation(assertion, file, line, func);
  std::abort();
}

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
