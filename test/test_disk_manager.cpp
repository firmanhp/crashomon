// test/test_disk_manager.cpp — unit tests for DiskManager

#include "daemon/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace crashomon {
namespace {

// RAII temp directory removed on destruction.
struct TempDir {
  std::filesystem::path path;

  TempDir() {
    char tmpl[] = "/tmp/crashomon_test_XXXXXX";
    const char* p = mkdtemp(tmpl);
    EXPECT_NE(p, nullptr);
    path = p ? p : "";
  }

  ~TempDir() {
    if (!path.empty()) std::filesystem::remove_all(path);
  }

  // Create a .dmp file of the given size (filled with zeros).
  std::filesystem::path CreateDmp(const std::string& name,
                                   size_t size_bytes = 1024) {
    auto fp = path / name;
    std::ofstream f(fp, std::ios::binary);
    std::vector<char> zeros(size_bytes, 0);
    f.write(zeros.data(), static_cast<std::streamsize>(size_bytes));
    return fp;
  }

  // Set the modification time of a file to `mtime`.
  static void SetMtime(const std::filesystem::path& fp, time_t mtime) {
    struct timespec times[2] = {{mtime, 0}, {mtime, 0}};
    EXPECT_EQ(::utimensat(AT_FDCWD, fp.c_str(), times, 0), 0);
  }
};

// ── GetTotalMinidumpSize ──────────────────────────────────────────────────────

TEST(DiskManagerTest, TotalSizeEmptyDirectory) {
  TempDir tmp;
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, 0u);
}

TEST(DiskManagerTest, TotalSizeSingleFile) {
  TempDir tmp;
  tmp.CreateDmp("a.dmp", 4096);
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, 4096u);
}

TEST(DiskManagerTest, TotalSizeMultipleFiles) {
  TempDir tmp;
  tmp.CreateDmp("a.dmp", 1024);
  tmp.CreateDmp("b.dmp", 2048);
  tmp.CreateDmp("c.dmp", 512);
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, 1024u + 2048u + 512u);
}

TEST(DiskManagerTest, TotalSizeIgnoresNonDmpFiles) {
  TempDir tmp;
  tmp.CreateDmp("a.dmp", 1024);
  // Create a non-.dmp file.
  std::ofstream(tmp.path / "notes.txt") << "hello";
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, 1024u);
}

TEST(DiskManagerTest, TotalSizeNonexistentDirectory) {
  auto size_or = GetTotalMinidumpSize("/nonexistent/path/that/does/not/exist");
  EXPECT_FALSE(size_or.ok());
}

// ── PruneMinidumps ────────────────────────────────────────────────────────────

TEST(DiskManagerTest, NoLimitsDoesNothing) {
  TempDir tmp;
  tmp.CreateDmp("a.dmp", 1024);
  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 0;
  cfg.max_age_seconds = 0;
  EXPECT_TRUE(PruneMinidumps(cfg).ok());
  // File still present.
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "a.dmp"));
}

TEST(DiskManagerTest, SizeLimitDeletesOldestFirst) {
  TempDir tmp;
  // Create files with different mtimes so oldest is deterministic.
  auto old = tmp.CreateDmp("old.dmp", 1024);
  auto newer = tmp.CreateDmp("newer.dmp", 1024);
  time_t now = time(nullptr);
  TempDir::SetMtime(old, now - 100);
  TempDir::SetMtime(newer, now - 10);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 1024; // allow only one file
  cfg.max_age_seconds = 0;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());

  // old.dmp should be deleted; newer.dmp should survive.
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "newer.dmp"));
}

TEST(DiskManagerTest, SizeLimitUnderBudgetDoesNothing) {
  TempDir tmp;
  tmp.CreateDmp("a.dmp", 512);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 1024 * 1024; // 1 MB — well above 512 bytes
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "a.dmp"));
}

TEST(DiskManagerTest, AgeLimit) {
  TempDir tmp;
  auto old = tmp.CreateDmp("old.dmp", 512);
  auto fresh = tmp.CreateDmp("fresh.dmp", 512);
  time_t now = time(nullptr);
  TempDir::SetMtime(old, now - 1000); // 1000 seconds ago
  TempDir::SetMtime(fresh, now - 10); // 10 seconds ago

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_age_seconds = 500; // delete anything older than 500s

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "fresh.dmp"));
}

TEST(DiskManagerTest, EmptyDirectoryWithLimits) {
  TempDir tmp;
  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 1024;
  cfg.max_age_seconds = 60;
  EXPECT_TRUE(PruneMinidumps(cfg).ok());
}

TEST(DiskManagerTest, SingleFileAtExactSizeBoundary) {
  TempDir tmp;
  tmp.CreateDmp("a.dmp", 1024);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 1024; // exact — should not prune
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "a.dmp"));
}

}  // namespace
}  // namespace crashomon
