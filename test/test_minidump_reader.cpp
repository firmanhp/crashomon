// test/test_minidump_reader.cpp — unit tests for MinidumpReader
//
// These tests require real .dmp fixture files produced by gen_fixtures.sh.
// If the fixtures directory is absent or a specific .dmp is missing, the
// relevant test is skipped with GTEST_SKIP() rather than failing.
//
// Fixture location (in order of precedence):
//   1. CRASHOMON_FIXTURES_DIR environment variable
//   2. A "fixtures" subdirectory next to the test binary

#include "daemon/minidump_reader.h"

#include <sys/stat.h>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"

namespace crashomon {
namespace {

// ── Fixture path helpers ────────────────────────────────────────────────────

// Returns the directory that holds the .dmp fixtures (may not exist).
std::filesystem::path FixturesDir() {
  const char* env = std::getenv("CRASHOMON_FIXTURES_DIR");
  if (env && *env != '\0') return env;

  // Fall back to a "fixtures" subdir next to the running test binary.
  // argv[0] is not easily portable from a test body, but
  // GTEST_BINARY_DIR is set by CMake via gtest_discover_tests().
  const char* bin_dir = std::getenv("GTEST_BINARY_DIR");
  if (bin_dir && *bin_dir != '\0') {
    return std::filesystem::path(bin_dir) / "fixtures";
  }

  // Last resort: relative path from the cwd (works when run from repo root).
  return std::filesystem::path("test") / "fixtures";
}

// Returns the path to a fixture file; an empty path signals "skip this test".
std::filesystem::path FixturePath(const std::string& name) {
  auto p = FixturesDir() / (name + ".dmp");
  if (!std::filesystem::exists(p)) return {};
  return p;
}

// ── Error-path tests (no fixture files required) ────────────────────────────

TEST(MinidumpReaderTest, NonexistentFile) {
  auto result = ReadMinidump("/nonexistent/path/does_not_exist.dmp");
  EXPECT_FALSE(result.ok());
}

TEST(MinidumpReaderTest, EmptyFile) {
  // Write a zero-byte temp file and confirm the parser rejects it.
  char tmpl[] = "/tmp/crashomon_empty_XXXXXX";
  int fd = mkstemp(tmpl);
  ASSERT_GE(fd, 0);
  ::close(fd);

  auto result = ReadMinidump(tmpl);
  EXPECT_FALSE(result.ok());

  ::unlink(tmpl);
}

TEST(MinidumpReaderTest, GarbageFile) {
  // Write random bytes and confirm the parser rejects it.
  char tmpl[] = "/tmp/crashomon_garbage_XXXXXX";
  int fd = mkstemp(tmpl);
  ASSERT_GE(fd, 0);
  const char junk[] = "not a minidump\x00\x01\x02\x03";
  ssize_t written = ::write(fd, junk, sizeof(junk) - 1);
  (void)written;
  ::close(fd);

  auto result = ReadMinidump(tmpl);
  EXPECT_FALSE(result.ok());

  ::unlink(tmpl);
}

// ── Fixture-based tests ─────────────────────────────────────────────────────

// Helper macro: skip the test if the named fixture is absent.
#define REQUIRE_FIXTURE(name, path_var)                              \
  auto path_var = FixturePath(name);                                 \
  if (path_var.empty()) {                                            \
    GTEST_SKIP() << "Fixture '" name ".dmp' not found in "          \
                 << FixturesDir() << " — run test/gen_fixtures.sh";  \
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
    if (name == "rip") { found_rip = true; break; }
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
  EXPECT_GE(result->threads.size(), 2u);
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
  for (const auto& t : result->threads) {
    if (t.is_crashing) ++crashing_count;
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
