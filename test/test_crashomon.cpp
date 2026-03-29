// test/test_crashomon.cpp — unit tests for crashomon configuration resolution.
//
// Covers GetEnv() and Resolve() from crashomon_internal.h.  These functions
// contain all the non-trivial logic in the client library; the sentry calls
// that follow are a thin pass-through and are exercised by integration tests.
//
// No sentry-native linkage is required here — crashomon_internal.h is all
// inline and has no dependency on <sentry.h>.

#include "lib/crashomon_internal.h"

#include <cstdlib>

#include "gtest/gtest.h"

namespace crashomon {
namespace {

// RAII guard: sets an environment variable on construction, restores the
// previous state (set or unset) on destruction.
struct ScopedEnv {
  const char* name;
  bool was_set;
  std::string old_value;

  ScopedEnv(const char* env_name, const char* value) : name(env_name) {
    if (const char* prev = std::getenv(env_name)) {
      was_set = true;
      old_value = prev;
    } else {
      was_set = false;
    }
    ::setenv(name, value, /*overwrite=*/1);
  }

  ~ScopedEnv() {
    if (was_set) {
      ::setenv(name, old_value.c_str(), /*overwrite=*/1);
    } else {
      ::unsetenv(name);
    }
  }
};

// Ensure both crashomon env vars are absent for tests that rely on defaults.
struct ClearCrashomonEnv {
  ClearCrashomonEnv() {
    ::unsetenv("CRASHOMON_DB_PATH");
    ::unsetenv("CRASHOMON_SOCKET_PATH");
  }
};

// ── Compiled-in defaults ─────────────────────────────────────────────────────

TEST(DefaultsTest, DbPathDefault) {
  EXPECT_EQ(kDefaultDbPath, "/var/crashomon");
}

TEST(DefaultsTest, SocketPathDefault) {
  EXPECT_EQ(kDefaultSocketPath, "/run/crashomon/handler.sock");
}

// ── GetEnv ───────────────────────────────────────────────────────────────────

TEST(GetEnvTest, ReturnsNulloptWhenAbsent) {
  ::unsetenv("CRASHOMON_TEST_GETENV");
  EXPECT_FALSE(GetEnv("CRASHOMON_TEST_GETENV").has_value());
}

TEST(GetEnvTest, ReturnsValueWhenPresent) {
  ScopedEnv e{"CRASHOMON_TEST_GETENV", "hello"};
  auto val = GetEnv("CRASHOMON_TEST_GETENV");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "hello");
}

TEST(GetEnvTest, ReturnsEmptyStringViewForEmptyVar) {
  ScopedEnv e{"CRASHOMON_TEST_GETENV", ""};
  auto val = GetEnv("CRASHOMON_TEST_GETENV");
  ASSERT_TRUE(val.has_value());
  EXPECT_TRUE(val->empty());
}

TEST(GetEnvTest, ReturnsStringViewNotOwningCopy) {
  ScopedEnv e{"CRASHOMON_TEST_GETENV", "test_value"};
  auto val = GetEnv("CRASHOMON_TEST_GETENV");
  ASSERT_TRUE(val.has_value());
  // The returned string_view should compare equal to the set value.
  EXPECT_EQ(std::string{*val}, "test_value");
}

// ── Resolve: defaults (no config, no env vars) ───────────────────────────────

TEST(ResolveTest, NullConfigNoEnvYieldsDefaults) {
  ClearCrashomonEnv clear;
  auto cfg = Resolve(nullptr);
  EXPECT_EQ(cfg.db_path, kDefaultDbPath);
  EXPECT_EQ(cfg.socket_path, kDefaultSocketPath);
}

TEST(ResolveTest, EmptyConfigNoEnvYieldsDefaults) {
  ClearCrashomonEnv clear;
  CrashomonConfig config{nullptr, nullptr};
  auto cfg = Resolve(&config);
  EXPECT_EQ(cfg.db_path, kDefaultDbPath);
  EXPECT_EQ(cfg.socket_path, kDefaultSocketPath);
}

// ── Resolve: environment variable override ───────────────────────────────────

TEST(ResolveTest, EnvDbPathOverridesDefault) {
  ClearCrashomonEnv clear;
  ScopedEnv e{"CRASHOMON_DB_PATH", "/tmp/crashes"};
  auto cfg = Resolve(nullptr);
  EXPECT_EQ(cfg.db_path, "/tmp/crashes");
  EXPECT_EQ(cfg.socket_path, kDefaultSocketPath);
}

TEST(ResolveTest, EnvHandlerPathOverridesDefault) {
  ClearCrashomonEnv clear;
  ScopedEnv e{"CRASHOMON_SOCKET_PATH", "/opt/myhandler"};
  auto cfg = Resolve(nullptr);
  EXPECT_EQ(cfg.db_path, kDefaultDbPath);
  EXPECT_EQ(cfg.socket_path, "/opt/myhandler");
}

TEST(ResolveTest, BothEnvVarsApplied) {
  ClearCrashomonEnv clear;
  ScopedEnv db{"CRASHOMON_DB_PATH", "/data/crashes"};
  ScopedEnv handler{"CRASHOMON_SOCKET_PATH", "/bin/handler"};
  auto cfg = Resolve(nullptr);
  EXPECT_EQ(cfg.db_path, "/data/crashes");
  EXPECT_EQ(cfg.socket_path, "/bin/handler");
}

// ── Resolve: explicit config takes highest precedence ────────────────────────

TEST(ResolveTest, ExplicitConfigOverridesEnvAndDefault) {
  ScopedEnv db{"CRASHOMON_DB_PATH", "/env/crashes"};
  ScopedEnv handler{"CRASHOMON_SOCKET_PATH", "/env/handler"};
  CrashomonConfig config{"/explicit/db", "/explicit/handler"};
  auto cfg = Resolve(&config);
  EXPECT_EQ(cfg.db_path, "/explicit/db");
  EXPECT_EQ(cfg.socket_path, "/explicit/handler");
}

TEST(ResolveTest, ExplicitDbPathOnlyLeavesHandlerToEnv) {
  ClearCrashomonEnv clear;
  ScopedEnv e{"CRASHOMON_SOCKET_PATH", "/env/handler"};
  CrashomonConfig config{"/explicit/db", nullptr};
  auto cfg = Resolve(&config);
  EXPECT_EQ(cfg.db_path, "/explicit/db");
  EXPECT_EQ(cfg.socket_path, "/env/handler");
}

TEST(ResolveTest, ExplicitHandlerPathOnlyLeavesDbToEnv) {
  ClearCrashomonEnv clear;
  ScopedEnv e{"CRASHOMON_DB_PATH", "/env/db"};
  CrashomonConfig config{nullptr, "/explicit/handler"};
  auto cfg = Resolve(&config);
  EXPECT_EQ(cfg.db_path, "/env/db");
  EXPECT_EQ(cfg.socket_path, "/explicit/handler");
}

TEST(ResolveTest, ExplicitDbPathOnlyLeavesHandlerToDefault) {
  ClearCrashomonEnv clear;
  CrashomonConfig config{"/explicit/db", nullptr};
  auto cfg = Resolve(&config);
  EXPECT_EQ(cfg.db_path, "/explicit/db");
  EXPECT_EQ(cfg.socket_path, kDefaultSocketPath);
}

TEST(ResolveTest, ExplicitHandlerPathOnlyLeavesDbToDefault) {
  ClearCrashomonEnv clear;
  CrashomonConfig config{nullptr, "/explicit/handler"};
  auto cfg = Resolve(&config);
  EXPECT_EQ(cfg.db_path, kDefaultDbPath);
  EXPECT_EQ(cfg.socket_path, "/explicit/handler");
}

// ── Resolve: result is an owned copy ─────────────────────────────────────────

TEST(ResolveTest, ResultIsIndependentOfEnvAfterResolve) {
  ClearCrashomonEnv clear;
  ScopedEnv e{"CRASHOMON_DB_PATH", "/original"};
  auto cfg = Resolve(nullptr);
  // Modify the env after resolving — result must not change.
  ::setenv("CRASHOMON_DB_PATH", "/modified", 1);
  EXPECT_EQ(cfg.db_path, "/original");
}

TEST(ResolveTest, ResultIsIndependentOfConfigPointerAfterResolve) {
  ClearCrashomonEnv clear;
  std::string db   = "/owned/db";
  std::string hdlr = "/owned/handler";
  CrashomonConfig config{db.c_str(), hdlr.c_str()};
  auto cfg = Resolve(&config);
  // Mutate original strings — result must not change.
  db   = "/mutated";
  hdlr = "/mutated";
  EXPECT_EQ(cfg.db_path, "/owned/db");
  EXPECT_EQ(cfg.socket_path, "/owned/handler");
}

}  // namespace
}  // namespace crashomon
