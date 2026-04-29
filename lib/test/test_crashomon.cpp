// test/test_crashomon.cpp — unit tests for crashomon configuration resolution.
//
// Covers GetEnv() and Resolve() from crashomon_internal.h.  These functions
// contain all the non-trivial logic in the client library; the crashpad calls
// that follow are a thin pass-through and are exercised by integration tests.
//
// No crashpad linkage is required here — crashomon_internal.h is all
// inline and has no dependency on crashpad headers.

#include <cstdlib>

#include "gtest/gtest.h"
#include "lib/crashomon_internal.h"

namespace crashomon {
namespace {

// RAII guard: sets an environment variable on construction, restores the
// previous state (set or unset) on destruction.
class ScopedEnv {
 public:
  // env_name and value are semantically
  // orthogonal (variable name vs its value); the class name makes the distinction clear.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  ScopedEnv(const char* env_name, const char* value) : name_(env_name) {
    if (const char* prev = std::getenv(env_name)) {
      was_set_ = true;
      old_value_ = prev;
    } else {
      was_set_ = false;
    }
    // setenv is in <cstdlib> which is included; include-cleaner FP.
    // NOLINTNEXTLINE(misc-include-cleaner)
    ::setenv(name_, value, /*overwrite=*/1);
  }

  ~ScopedEnv() {
    if (was_set_) {
      ::setenv(name_, old_value_.c_str(),
               /*overwrite=*/1);  // NOLINT(misc-include-cleaner) — setenv is in <cstdlib> which is
                                  // included; false positive from include-cleaner.
    } else {
      ::unsetenv(name_);  // NOLINT(misc-include-cleaner) — unsetenv is in <cstdlib> which is
                          // included; false positive from include-cleaner.
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;
  ScopedEnv(ScopedEnv&&) = delete;
  ScopedEnv& operator=(ScopedEnv&&) = delete;

 private:
  const char* name_;
  bool was_set_{false};
  std::string old_value_;
};

// Ensure both crashomon env vars are absent for tests that rely on defaults.
class ClearCrashomonEnv {
 public:
  ClearCrashomonEnv() {
    ::unsetenv("CRASHOMON_DB_PATH");
    ::unsetenv("CRASHOMON_SOCKET_PATH");
  }

  ~ClearCrashomonEnv() = default;
  ClearCrashomonEnv(const ClearCrashomonEnv&) = delete;
  ClearCrashomonEnv& operator=(const ClearCrashomonEnv&) = delete;
  ClearCrashomonEnv(ClearCrashomonEnv&&) = delete;
  ClearCrashomonEnv& operator=(ClearCrashomonEnv&&) = delete;
};

// ── Compiled-in defaults ─────────────────────────────────────────────────────

TEST(DefaultsTest, DbPathDefault) { EXPECT_EQ(kDefaultDbPath, "/var/crashomon"); }

TEST(DefaultsTest, SocketPathDefault) {
  EXPECT_EQ(kDefaultSocketPath, "/run/crashomon/handler.sock");
}

// ── GetEnv ───────────────────────────────────────────────────────────────────

TEST(GetEnvTest, ReturnsNulloptWhenAbsent) {
  ::unsetenv("CRASHOMON_TEST_GETENV");
  EXPECT_FALSE(GetEnv("CRASHOMON_TEST_GETENV").has_value());
}

TEST(GetEnvTest, ReturnsValueWhenPresent) {
  const ScopedEnv env{"CRASHOMON_TEST_GETENV", "hello"};
  const auto val = GetEnv("CRASHOMON_TEST_GETENV");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "hello");  // NOLINT(bugprone-unchecked-optional-access) — checked by preceding
                             // ASSERT_TRUE; analyzer cannot see through GoogleTest macros.
}

TEST(GetEnvTest, ReturnsEmptyStringViewForEmptyVar) {
  const ScopedEnv env{"CRASHOMON_TEST_GETENV", ""};
  const auto val = GetEnv("CRASHOMON_TEST_GETENV");
  ASSERT_TRUE(val.has_value());
  EXPECT_TRUE(val->empty());  // NOLINT(bugprone-unchecked-optional-access) — checked by preceding
                              // ASSERT_TRUE; analyzer cannot see through GoogleTest macros.
}

TEST(GetEnvTest, ReturnsStringViewNotOwningCopy) {
  const ScopedEnv env{"CRASHOMON_TEST_GETENV", "test_value"};
  const auto val = GetEnv("CRASHOMON_TEST_GETENV");
  ASSERT_TRUE(val.has_value());
  // The returned string_view should compare equal to the set value.
  // Checked by preceding ASSERT_TRUE; analyzer cannot see through GoogleTest macros.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(std::string{*val}, "test_value");
}

// ── Resolve: defaults (no env vars) ─────────────────────────────────────────

TEST(ResolveTest, NoEnvYieldsDefaults) {
  const ClearCrashomonEnv clear;
  const auto cfg = Resolve();
  EXPECT_EQ(cfg.db_path, kDefaultDbPath);
  EXPECT_EQ(cfg.socket_path, kDefaultSocketPath);
}

// ── Resolve: environment variable override ───────────────────────────────────

TEST(ResolveTest, EnvDbPathOverridesDefault) {
  const ClearCrashomonEnv clear;
  const ScopedEnv env{"CRASHOMON_DB_PATH", "/tmp/crashes"};
  const auto cfg = Resolve();
  EXPECT_EQ(cfg.db_path, "/tmp/crashes");
  EXPECT_EQ(cfg.socket_path, kDefaultSocketPath);
}

TEST(ResolveTest, EnvHandlerPathOverridesDefault) {
  const ClearCrashomonEnv clear;
  const ScopedEnv env{"CRASHOMON_SOCKET_PATH", "/opt/myhandler"};
  const auto cfg = Resolve();
  EXPECT_EQ(cfg.db_path, kDefaultDbPath);
  EXPECT_EQ(cfg.socket_path, "/opt/myhandler");
}

TEST(ResolveTest, BothEnvVarsApplied) {
  const ClearCrashomonEnv clear;
  const ScopedEnv db_env{"CRASHOMON_DB_PATH", "/data/crashes"};
  const ScopedEnv handler_env{"CRASHOMON_SOCKET_PATH", "/bin/handler"};
  const auto cfg = Resolve();
  EXPECT_EQ(cfg.db_path, "/data/crashes");
  EXPECT_EQ(cfg.socket_path, "/bin/handler");
}

// ── Resolve: result is an owned copy ─────────────────────────────────────────

TEST(ResolveTest, ResultIsIndependentOfEnvAfterResolve) {
  const ClearCrashomonEnv clear;
  const ScopedEnv env{"CRASHOMON_DB_PATH", "/original"};
  const auto cfg = Resolve();
  // Modify the env after resolving — result must not change.
  ::setenv("CRASHOMON_DB_PATH", "/modified", 1);
  EXPECT_EQ(cfg.db_path, "/original");
}

}  // namespace
}  // namespace crashomon
