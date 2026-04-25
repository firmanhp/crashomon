// daemon/test/test_worker.cpp — unit tests for ProcessNewMinidump
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
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "daemon/tombstone/minidump_reader.h"

namespace crashomon {
namespace {

using ::testing::_;
using ::testing::Return;

// ── Mock ──────────────────────────────────────────────────────────────────────

class MockTombstone : public ITombstone {
 public:
  MOCK_METHOD(absl::StatusOr<MinidumpInfo>, ReadMinidump, (const std::string& path), (override));
  MOCK_METHOD(std::string, FormatTombstone, (const MinidumpInfo& info), (override));
};

// ── Test constants ────────────────────────────────────────────────────────────

constexpr size_t kBytes1K = 1024;
constexpr uint64_t kBytes1M = static_cast<uint64_t>(1024) * 1024;

constexpr time_t kAge10s = 10;
constexpr time_t kAge50s = 50;
constexpr time_t kAge100s = 100;
constexpr time_t kAge200s = 200;
constexpr time_t kAge300s = 300;

// ── Helpers ───────────────────────────────────────────────────────────────────

struct TempDir {
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

  static void SetMtime(const std::filesystem::path& file_path, time_t mtime) {
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    struct timespec times[2] = {{mtime, 0}, {mtime, 0}};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    EXPECT_EQ(::utimensat(AT_FDCWD, file_path.c_str(), times, 0), 0);
  }

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

MinidumpInfo MakeInfo() {
  MinidumpInfo info;
  info.process_name = "test_process";
  info.signal_info = "SIGSEGV";
  info.fault_addr = 0x4;
  return info;
}

// ── ProcessNewMinidump: error path ────────────────────────────────────────────

// Core regression test for the original bug: pruning must run even when
// ReadMinidump fails, so a full disk that causes partial writes can still
// reclaim space.
TEST(WorkerTest, PrunesEvenOnReadError) {
  const TempDir dbt;

  const time_t now = time(nullptr);
  auto old1 = dbt.CreateDmp("old1.dmp");
  auto old2 = dbt.CreateDmp("old2.dmp");
  auto keep = dbt.CreateDmp("keep.dmp");
  TempDir::SetMtime(old1, now - kAge300s);
  TempDir::SetMtime(old2, now - kAge200s);
  TempDir::SetMtime(keep, now - kAge100s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1K;

  MockTombstone tombstone;
  EXPECT_CALL(tombstone, ReadMinidump(_))
      .WillOnce(Return(absl::NotFoundError("mock: file not found")));
  EXPECT_CALL(tombstone, FormatTombstone(_)).Times(0);

  WorkerState state;
  ProcessNewMinidump("/nonexistent/partial.dmp", state, cfg, "", tombstone);

  EXPECT_EQ(dbt.CountDmpFiles(), 1U);
  EXPECT_TRUE(std::filesystem::exists(dbt.path / "pending" / "keep.dmp"));
  EXPECT_FALSE(std::filesystem::exists(dbt.path / "pending" / "old1.dmp"));
  EXPECT_FALSE(std::filesystem::exists(dbt.path / "pending" / "old2.dmp"));
}

TEST(WorkerTest, ReadErrorOnEmptyDbIsOk) {
  const TempDir dbt;

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1K;

  MockTombstone tombstone;
  EXPECT_CALL(tombstone, ReadMinidump(_))
      .WillOnce(Return(absl::NotFoundError("mock: file not found")));
  EXPECT_CALL(tombstone, FormatTombstone(_)).Times(0);

  WorkerState state;
  ProcessNewMinidump("/nonexistent/partial.dmp", state, cfg, "", tombstone);

  EXPECT_EQ(dbt.CountDmpFiles(), 0U);
}

TEST(WorkerTest, ReadErrorWithNoLimitsIsOk) {
  const TempDir dbt;
  dbt.CreateDmp("a.dmp");

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();

  MockTombstone tombstone;
  EXPECT_CALL(tombstone, ReadMinidump(_))
      .WillOnce(Return(absl::NotFoundError("mock: file not found")));
  EXPECT_CALL(tombstone, FormatTombstone(_)).Times(0);

  WorkerState state;
  ProcessNewMinidump("/nonexistent/partial.dmp", state, cfg, "", tombstone);

  EXPECT_EQ(dbt.CountDmpFiles(), 1U);
}

// ── ProcessNewMinidump: success path ──────────────────────────────────────────

TEST(WorkerTest, PrunesOnSuccessPath) {
  const TempDir dbt;
  const time_t now = time(nullptr);
  auto old1 = dbt.CreateDmp("old1.dmp");
  auto old2 = dbt.CreateDmp("old2.dmp");
  TempDir::SetMtime(old1, now - kAge300s);
  TempDir::SetMtime(old2, now - kAge200s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.max_bytes = kBytes1M;

  MockTombstone tombstone;
  EXPECT_CALL(tombstone, ReadMinidump(_)).WillOnce(Return(MakeInfo()));
  EXPECT_CALL(tombstone, FormatTombstone(_)).WillOnce(Return(""));

  WorkerState state;
  ProcessNewMinidump("/fake.dmp", state, cfg, "", tombstone);

  EXPECT_EQ(state.rate_limit_map.size(), 1U);
  EXPECT_EQ(dbt.CountDmpFiles(), 2U);
}

// ── ProcessNewMinidump: rate-limit path ───────────────────────────────────────

// When the same crash signature arrives within kRateLimitWindow, the second
// call is suppressed (no FormatTombstone) but pruning still runs.
TEST(WorkerTest, PrunesOnRateLimitPath) {
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
  cfg.max_bytes = kBytes1K;

  MockTombstone tombstone;
  // ReadMinidump succeeds both times with the same crash signature.
  EXPECT_CALL(tombstone, ReadMinidump(_)).WillRepeatedly(Return(MakeInfo()));
  // FormatTombstone is called only on the first (non-rate-limited) call.
  EXPECT_CALL(tombstone, FormatTombstone(_)).WillOnce(Return(""));

  WorkerState state;

  // First call — processes normally, prunes db to 1 file.
  ProcessNewMinidump("/fake.dmp", state, cfg, "", tombstone);
  EXPECT_EQ(dbt.CountDmpFiles(), 1U);

  // Refill so the second call has something to prune.
  auto new1 = dbt.CreateDmp("new1.dmp");
  auto new2 = dbt.CreateDmp("new2.dmp");
  TempDir::SetMtime(new1, now - kAge50s);
  TempDir::SetMtime(new2, now - kAge10s);

  // Second call within kRateLimitWindow — rate-limited, but pruning still runs.
  ProcessNewMinidump("/fake.dmp", state, cfg, "", tombstone);
  EXPECT_EQ(dbt.CountDmpFiles(), 1U);
}

}  // namespace
}  // namespace crashomon
