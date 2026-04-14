// test/test_worker.cpp — unit tests for ProcessNewMinidump
//
// The historical bug: in the early-return error path of ProcessNewMinidump
// (commit 88d0d93), PruneMinidumps was not called when ReadMinidump failed.
// When a disk fills up, Crashpad writes partial dumps that fail to parse;
// without pruning on the error path, the disk stayed full indefinitely.
//
// These tests verify that PruneMinidumps is called unconditionally — on the
// read-error path, the rate-limit path, and the normal success path — by
// observing whether excess .dmp files in db_path are actually removed.

#include <fcntl.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "daemon/disk_manager.h"
#include "daemon/worker.h"
#include "gtest/gtest.h"

namespace crashomon {
namespace {

// ── Test constants ────────────────────────────────────────────────────────────

constexpr size_t kBytes1K = 1024;
constexpr uint64_t kBytes1M = static_cast<uint64_t>(1024) * 1024;

// Mtime offsets (seconds before now) for deterministic oldest-first ordering.
constexpr time_t kAge10s = 10;
constexpr time_t kAge50s = 50;
constexpr time_t kAge100s = 100;
constexpr time_t kAge200s = 200;
constexpr time_t kAge300s = 300;

// ── Helpers ───────────────────────────────────────────────────────────────────

// RAII temp directory removed on destruction.
struct TempDir {
  // test-only RAII struct; public member intentional.
  // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
  std::filesystem::path path;

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  TempDir() {
    std::string tmpl = "/tmp/crashomon_worker_test_XXXXXX";
    // NOLINTNEXTLINE(misc-include-cleaner)
    const char* dir_path = mkdtemp(tmpl.data());
    EXPECT_NE(dir_path, nullptr);
    path = (dir_path != nullptr) ? dir_path : "";
  }

  ~TempDir() {
    if (!path.empty()) {
      std::filesystem::remove_all(path);
    }
  }

  // Create a .dmp file in the pending/ subdirectory (where Crashpad places
  // completed minidumps). Returns the path.
  // NOLINTNEXTLINE(modernize-use-nodiscard)
  std::filesystem::path CreateDmp(const std::string& name, size_t size_bytes = kBytes1K) const {
    std::filesystem::create_directories(path / "pending");
    auto file_path = path / "pending" / name;
    // NOLINTNEXTLINE(misc-include-cleaner)
    std::ofstream ofs(file_path, std::ios::binary);
    std::vector<char> zeros(size_bytes, 0);
    ofs.write(zeros.data(), static_cast<std::streamsize>(size_bytes));
    return file_path;
  }

  // Set the modification time of a file.
  static void SetMtime(const std::filesystem::path& file_path, time_t mtime) {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    struct timespec times[2] = {{mtime, 0}, {mtime, 0}};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    EXPECT_EQ(::utimensat(AT_FDCWD, file_path.c_str(), times, 0), 0);
  }

  // Count .dmp files in pending/ (the directory PruneMinidumps operates on).
  size_t CountDmpFiles() const {
    const auto pending = path / "pending";
    if (!std::filesystem::is_directory(pending)) {
      return 0;
    }
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(pending)) {
      if (entry.path().extension() == ".dmp") {
        ++count;
      }
    }
    return count;
  }
};

// Returns the directory containing the .dmp fixture files, or empty path.
std::filesystem::path FixturesDir() {
  const char* env = std::getenv("CRASHOMON_FIXTURES_DIR");
  if (env != nullptr && *env != '\0') {
    return env;
  }
  const char* bin_dir = std::getenv("GTEST_BINARY_DIR");
  if (bin_dir != nullptr && *bin_dir != '\0') {
    return std::filesystem::path(bin_dir) / "fixtures";
  }
  return std::filesystem::path("test") / "fixtures";
}

std::filesystem::path FixturePath(const std::string& name) {
  auto fixture_path = FixturesDir() / (name + ".dmp");
  return std::filesystem::exists(fixture_path) ? fixture_path : std::filesystem::path{};
}

// ── ProcessNewMinidump: error path ────────────────────────────────────────────

// Core regression test for the original bug.
//
// When ReadMinidump fails (e.g. a partial dump written to a full disk),
// the old code returned early without calling PruneMinidumps.  This test
// verifies that excess .dmp files are pruned even when the incoming path
// is unreadable.
TEST(WorkerTest, PrunesEvenOnReadError) {
  const TempDir dbt;

  // Fill db_path with three 1 KB files; total = 3 KB, limit = 1 KB.
  const time_t now = time(nullptr);
  auto old1 = dbt.CreateDmp("old1.dmp");
  auto old2 = dbt.CreateDmp("old2.dmp");
  auto keep = dbt.CreateDmp("keep.dmp");
  TempDir::SetMtime(old1, now - kAge300s);
  TempDir::SetMtime(old2, now - kAge200s);
  TempDir::SetMtime(keep, now - kAge100s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1K;  // allows only one file

  WorkerState state;
  // Pass a path that does not exist — ReadMinidump will fail.
  ProcessNewMinidump("/nonexistent/partial.dmp", state, cfg, "");

  // Pruning must have run: only the newest file should remain.
  EXPECT_EQ(dbt.CountDmpFiles(), 1U);
  EXPECT_TRUE(std::filesystem::exists(dbt.path / "pending" / "keep.dmp"));
  EXPECT_FALSE(std::filesystem::exists(dbt.path / "pending" / "old1.dmp"));
  EXPECT_FALSE(std::filesystem::exists(dbt.path / "pending" / "old2.dmp"));
}

// An empty db_path (no files to prune) must not error on the read-error path.
TEST(WorkerTest, ReadErrorOnEmptyDbIsOk) {
  const TempDir dbt;

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1K;

  WorkerState state;
  // Should not crash or return an error — just log and continue.
  ProcessNewMinidump("/nonexistent/partial.dmp", state, cfg, "");

  EXPECT_EQ(dbt.CountDmpFiles(), 0U);
}

// With no limits configured, a read error must still not crash or error.
TEST(WorkerTest, ReadErrorWithNoLimitsIsOk) {
  const TempDir dbt;
  dbt.CreateDmp("a.dmp");  // placed in pending/ by CreateDmp

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  // max_bytes = 0, max_age_seconds = 0 → no pruning, but no error either.

  WorkerState state;
  ProcessNewMinidump("/nonexistent/partial.dmp", state, cfg, "");

  // File untouched — no pruning configured.
  EXPECT_EQ(dbt.CountDmpFiles(), 1U);
}

// ── ProcessNewMinidump: success path (fixture-gated) ─────────────────────────

// On the normal success path, excess .dmp files must also be pruned.
TEST(WorkerTest, PrunesOnSuccessPath) {
  const auto fixture = FixturePath("segfault");
  if (fixture.empty()) {
    GTEST_SKIP() << "segfault.dmp fixture not available; run gen_fixtures.sh";
  }

  const TempDir dbt;
  const time_t now = time(nullptr);
  auto old1 = dbt.CreateDmp("old1.dmp");
  auto old2 = dbt.CreateDmp("old2.dmp");
  TempDir::SetMtime(old1, now - kAge300s);
  TempDir::SetMtime(old2, now - kAge200s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1M;  // large enough to keep old files …

  WorkerState state;
  // Process a real minidump — tombstone goes to stdout.
  ProcessNewMinidump(fixture.string(), state, cfg, "");

  // The fixture is not in db_path, but pruning still ran without error.
  EXPECT_TRUE(state.rate_limit_map.size() == 1U);
  EXPECT_EQ(dbt.CountDmpFiles(), 2U);  // old files untouched (under budget)
}

// ── ProcessNewMinidump: rate-limit path (fixture-gated) ──────────────────────

// When a crash is rate-limited (same signature within 30 s), the second call
// must still prune — the dump file was written to disk and counts against the
// size budget.
TEST(WorkerTest, PrunesOnRateLimitPath) {
  const auto fixture = FixturePath("segfault");
  if (fixture.empty()) {
    GTEST_SKIP() << "segfault.dmp fixture not available; run gen_fixtures.sh";
  }

  const TempDir dbt;
  const time_t now = time(nullptr);
  auto old1 = dbt.CreateDmp("old1.dmp");
  auto old2 = dbt.CreateDmp("old2.dmp");
  auto old3 = dbt.CreateDmp("old3.dmp");
  TempDir::SetMtime(old1, now - kAge300s);
  TempDir::SetMtime(old2, now - kAge200s);
  TempDir::SetMtime(old3, now - kAge100s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1K;  // allows only one file

  WorkerState state;

  // First call — processes normally, prunes db.
  ProcessNewMinidump(fixture.string(), state, cfg, "");
  EXPECT_EQ(dbt.CountDmpFiles(), 1U);

  // Refill db so the second call has something to prune.
  auto new1 = dbt.CreateDmp("new1.dmp");
  auto new2 = dbt.CreateDmp("new2.dmp");
  TempDir::SetMtime(new1, now - kAge50s);
  TempDir::SetMtime(new2, now - kAge10s);

  // Second call within 30 s — same crash signature → rate-limited.
  // Pruning must still run.
  ProcessNewMinidump(fixture.string(), state, cfg, "");
  EXPECT_EQ(dbt.CountDmpFiles(), 1U);
}

}  // namespace
}  // namespace crashomon
