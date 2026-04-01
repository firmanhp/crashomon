// test/test_disk_manager.cpp — unit tests for DiskManager

#include <fcntl.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

#include "daemon/disk_manager.h"
#include "gtest/gtest.h"

namespace crashomon {
namespace {

// ── Test constants ────────────────────────────────────────────────────────────
constexpr size_t kBytes512 = 512;
constexpr size_t kBytes1K = 1024;
constexpr size_t kBytes2K = 2048;
constexpr size_t kBytes4K = 4096;
constexpr uint64_t kBytes1M = static_cast<uint64_t>(1024) * 1024;
constexpr uint32_t kAge10s = 10;
constexpr uint32_t kAge60s = 60;
constexpr uint32_t kAge100s = 100;
constexpr uint32_t kAge500s = 500;
constexpr uint32_t kAge1000s = 1000;

// RAII temp directory removed on destruction.
struct TempDir {
  // test-only RAII struct with
  // single data member; struct keyword signals intentional public access.
  // NOLINTNEXTLINE(misc-non-private-member-variables-in-classes)
  std::filesystem::path path;

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  TempDir() {
    // std::string provides a mutable char buffer for mkdtemp.
    std::string tmpl = "/tmp/crashomon_test_XXXXXX";
    // mkdtemp comes via <cstdlib> transitively; include-cleaner FP.
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

  // Create a .dmp file of the given size (filled with zeros). Returns the path.
  // return value is
  // intentionally optional (side-effect callers may discard); conventional return type notation is
  // clearer per Google Style Guide.
  // NOLINTNEXTLINE(modernize-use-nodiscard)
  std::filesystem::path CreateDmp(const std::string& name, size_t size_bytes = kBytes1K) const {
    auto file_path = path / name;
    // std::ios/std::streamsize come from <ios> which is included; include-cleaner FPs.
    // NOLINTNEXTLINE(misc-include-cleaner)
    std::ofstream ofs(file_path, std::ios::binary);
    std::vector<char> zeros(size_bytes, 0);
    ofs.write(zeros.data(), static_cast<std::streamsize>(size_bytes));
    return file_path;
  }

  // Set the modification time of a file to `mtime`.
  static void SetMtime(const std::filesystem::path& file_path, time_t mtime) {
    // utimensat
    // requires a raw C array of timespec; std::array not accepted by this POSIX API.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    struct timespec times[2] = {{mtime, 0}, {mtime, 0}};
    // passing times[] to
    // utimensat which expects a pointer; inherent to POSIX API.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    EXPECT_EQ(::utimensat(AT_FDCWD, file_path.c_str(), times, 0), 0);
  }
};

// ── GetTotalMinidumpSize ──────────────────────────────────────────────────────

TEST(DiskManagerTest, TotalSizeEmptyDirectory) {
  const TempDir tmp;
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, 0U);
}

TEST(DiskManagerTest, TotalSizeSingleFile) {
  const TempDir tmp;
  tmp.CreateDmp("a.dmp", kBytes4K);
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, kBytes4K);
}

TEST(DiskManagerTest, TotalSizeMultipleFiles) {
  const TempDir tmp;
  tmp.CreateDmp("a.dmp", kBytes1K);
  tmp.CreateDmp("b.dmp", kBytes2K);
  tmp.CreateDmp("c.dmp", kBytes512);
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, kBytes1K + kBytes2K + kBytes512);
}

TEST(DiskManagerTest, TotalSizeIgnoresNonDmpFiles) {
  const TempDir tmp;
  tmp.CreateDmp("a.dmp", kBytes1K);
  // Create a non-.dmp file.
  std::ofstream(tmp.path / "notes.txt") << "hello";
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, kBytes1K);
}

TEST(DiskManagerTest, TotalSizeNonexistentDirectory) {
  auto size_or = GetTotalMinidumpSize("/nonexistent/path/that/does/not/exist");
  EXPECT_FALSE(size_or.ok());
}

// ── PruneMinidumps ────────────────────────────────────────────────────────────

TEST(DiskManagerTest, NoLimitsDoesNothing) {
  const TempDir tmp;
  tmp.CreateDmp("a.dmp", kBytes1K);
  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 0;
  cfg.max_age_seconds = 0;
  EXPECT_TRUE(PruneMinidumps(cfg).ok());
  // File still present.
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "a.dmp"));
}

TEST(DiskManagerTest, SizeLimitDeletesOldestFirst) {
  const TempDir tmp;
  // Create files with different mtimes so oldest is deterministic.
  auto old = tmp.CreateDmp("old.dmp", kBytes1K);
  auto newer = tmp.CreateDmp("newer.dmp", kBytes1K);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old, now - kAge100s);
  TempDir::SetMtime(newer, now - kAge10s);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1K;  // allow only one file
  cfg.max_age_seconds = 0;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());

  // old.dmp should be deleted; newer.dmp should survive.
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "newer.dmp"));
}

TEST(DiskManagerTest, SizeLimitUnderBudgetDoesNothing) {
  const TempDir tmp;
  tmp.CreateDmp("a.dmp", kBytes512);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1M;  // 1 MB — well above 512 bytes
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "a.dmp"));
}

TEST(DiskManagerTest, AgeLimit) {
  const TempDir tmp;
  auto old = tmp.CreateDmp("old.dmp", kBytes512);
  auto fresh = tmp.CreateDmp("fresh.dmp", kBytes512);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old, now - kAge1000s);  // 1000 seconds ago
  TempDir::SetMtime(fresh, now - kAge10s);  // 10 seconds ago

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_age_seconds = kAge500s;  // delete anything older than 500s

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "fresh.dmp"));
}

TEST(DiskManagerTest, EmptyDirectoryWithLimits) {
  const TempDir tmp;
  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1K;
  cfg.max_age_seconds = kAge60s;
  EXPECT_TRUE(PruneMinidumps(cfg).ok());
}

TEST(DiskManagerTest, SingleFileAtExactSizeBoundary) {
  const TempDir tmp;
  tmp.CreateDmp("a.dmp", kBytes1K);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1K;  // exact — should not prune
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "a.dmp"));
}

}  // namespace
}  // namespace crashomon
