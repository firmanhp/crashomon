// test/test_minidump_reader.cpp — unit tests for MinidumpReader
//
// These tests require real .dmp fixture files produced by gen_fixtures.sh.
// If the fixtures directory is absent or a specific .dmp is missing, the
// relevant test is skipped with GTEST_SKIP() rather than failing.
//
// Fixture location (in order of precedence):
//   1. CRASHOMON_FIXTURES_DIR environment variable
//   2. A "fixtures" subdirectory next to the test binary

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "daemon/minidump_reader.h"
#include "gtest/gtest.h"

namespace crashomon {
namespace {

// ── Fixture path helpers ────────────────────────────────────────────────────

// Returns the directory that holds the .dmp fixtures (may not exist).
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::filesystem::path FixturesDir() {
  const char* env = std::getenv("CRASHOMON_FIXTURES_DIR");
  if (env != nullptr && *env != '\0') {
    return env;
  }

  // Fall back to a "fixtures" subdir next to the running test binary.
  // argv[0] is not easily portable from a test body, but
  // GTEST_BINARY_DIR is set by CMake via gtest_discover_tests().
  const char* bin_dir = std::getenv("GTEST_BINARY_DIR");
  if (bin_dir != nullptr && *bin_dir != '\0') {
    return std::filesystem::path(bin_dir) / "fixtures";
  }

  // Last resort: relative path from the cwd (works when run from repo root).
  return std::filesystem::path("test") / "fixtures";
}

// Returns the path to a fixture file; an empty path signals "skip this test".
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::filesystem::path FixturePath(const std::string& name) {
  auto fixture_path = FixturesDir() / (name + ".dmp");  // non-const to enable move on return
  if (!std::filesystem::exists(fixture_path)) {
    return {};
  }
  return fixture_path;
}

// ── Error-path tests (no fixture files required) ────────────────────────────

TEST(MinidumpReaderTest, NonexistentFile) {
  auto result = ReadMinidump("/nonexistent/path/does_not_exist.dmp");
  EXPECT_FALSE(result.ok());
}

TEST(MinidumpReaderTest, EmptyFile) {
  // Write a zero-byte temp file and confirm the parser rejects it.
  // std::string provides a mutable buffer for mkstemp.
  std::string tmpl = "/tmp/crashomon_empty_XXXXXX";
  // fd is the universal POSIX idiom for file
  // descriptor; renaming would reduce clarity.
  // NOLINTNEXTLINE(readability-identifier-length)
  const int fd =
      mkstemp(tmpl.data());  // NOLINT(misc-include-cleaner) — mkstemp comes via <cstdlib> which is
                             // included; false positive from include-cleaner.
  ASSERT_GE(fd, 0);
  ::close(fd);

  auto result = ReadMinidump(tmpl);
  EXPECT_FALSE(result.ok());

  ::unlink(tmpl.c_str());
}

TEST(MinidumpReaderTest, GarbageFile) {
  // Write random bytes and confirm the parser rejects it.
  // std::string provides a mutable buffer for mkstemp.
  std::string tmpl = "/tmp/crashomon_garbage_XXXXXX";
  // fd is the universal POSIX idiom for file
  // descriptor; renaming would reduce clarity.
  // NOLINTNEXTLINE(readability-identifier-length)
  const int fd =
      mkstemp(tmpl.data());  // NOLINT(misc-include-cleaner) — mkstemp comes via <cstdlib> which is
                             // included; false positive from include-cleaner.
  ASSERT_GE(fd, 0);
  // std::string with length constructor preserves embedded nulls.
  constexpr size_t junk_size = 18;
  const std::string junk{"not a minidump\x00\x01\x02\x03", junk_size};
  const ssize_t written = ::write(
      fd, junk.data(), junk.size());  // NOLINT(misc-include-cleaner) — ssize_t comes via <unistd.h>
                                      // which is included; false positive from include-cleaner.
  (void)written;
  ::close(fd);

  auto result = ReadMinidump(tmpl);
  EXPECT_FALSE(result.ok());

  ::unlink(tmpl.c_str());
}

// ── Fixture-based tests ─────────────────────────────────────────────────────

// Helper macro: skip the test if the named fixture is absent.
// Must be a macro so the variable declaration and GTEST_SKIP() appear at call-site scope.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#define REQUIRE_FIXTURE(name, path_var)                                       \
  auto path_var = FixturePath(name); /* NOLINT(bugprone-macro-parentheses) */ \
  if ((path_var).empty()) {                                                   \
    GTEST_SKIP() << "Fixture '" name ".dmp' not found in " << FixturesDir()   \
                 << " — run test/gen_fixtures.sh";                            \
  }

// ── segfault.dmp ─────────────────────────────────────────────────────────────

TEST(MinidumpReaderTest, SegfaultFixture_ParsesOk) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(MinidumpReaderTest, SegfaultFixture_HasSigsegv) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  // signal_info format: "SIGSEGV / SEGV_MAPERR" or similar.
  EXPECT_NE(result->signal_info.find("SIGSEGV"), std::string::npos)
      << "signal_info: " << result->signal_info;
}

TEST(MinidumpReaderTest, SegfaultFixture_HasAtLeastOneThread) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->threads.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_CrashingThreadFirst) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

TEST(MinidumpReaderTest, SegfaultFixture_CrashingThreadHasFrames) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_FALSE(result->threads[0].frames.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_CrashingThreadHasRegisters) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  // AMD64 registers should be populated for the crashing thread.
  EXPECT_FALSE(result->threads[0].registers.empty());
  // RIP must be present.
  bool found_rip = false;
  for (const auto& [name, val] : result->threads[0].registers) {
    if (name == "rip") {
      found_rip = true;
      break;
    }
  }
  EXPECT_TRUE(found_rip) << "Expected 'rip' in crashing thread registers";
}

TEST(MinidumpReaderTest, SegfaultFixture_HasModules) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->modules.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_ProcessNameNonEmpty) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->process_name.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_TimestampNonEmpty) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->timestamp.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_MinidumpPathPreserved) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->minidump_path, path.string());
}

// ── abort.dmp ────────────────────────────────────────────────────────────────

TEST(MinidumpReaderTest, AbortFixture_ParsesOk) {
  REQUIRE_FIXTURE("abort", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(MinidumpReaderTest, AbortFixture_HasSigabrt) {
  REQUIRE_FIXTURE("abort", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_NE(result->signal_info.find("SIGABRT"), std::string::npos)
      << "signal_info: " << result->signal_info;
}

TEST(MinidumpReaderTest, AbortFixture_HasCrashingThread) {
  REQUIRE_FIXTURE("abort", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

// ── multithread.dmp ──────────────────────────────────────────────────────────

TEST(MinidumpReaderTest, MultithreadFixture_ParsesOk) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(MinidumpReaderTest, MultithreadFixture_HasMultipleThreads) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  // The program spawns at least 2 threads (idle + crash) plus the main thread.
  EXPECT_GE(result->threads.size(), 2U);
}

TEST(MinidumpReaderTest, MultithreadFixture_CrashingThreadIsFirst) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

TEST(MinidumpReaderTest, MultithreadFixture_OnlyOneThreadIsCrashing) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  int crashing_count = 0;
  for (const auto& thr : result->threads) {
    if (thr.is_crashing) {
      ++crashing_count;
    }
  }
  EXPECT_EQ(crashing_count, 1);
}

TEST(MinidumpReaderTest, MultithreadFixture_NonCrashingThreadsHaveNoRegisters) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  for (size_t i = 1; i < result->threads.size(); ++i) {
    EXPECT_TRUE(result->threads[i].registers.empty())
        << "Thread " << i << " is not the crashing thread but has registers";
  }
}

}  // namespace
}  // namespace crashomon
