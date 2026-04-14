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
constexpr size_t kBytes64 = 64;
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

  // Create a .dmp file directly in the temp dir (used for export-path tests and
  // non-pending placement checks).
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

  // Create a .dmp file inside pending/ (where Crashpad places completed minidumps).
  // NOLINTNEXTLINE(modernize-use-nodiscard)
  std::filesystem::path CreateDmpInPending(const std::string& name,
                                           size_t size_bytes = kBytes1K) const {
    std::filesystem::create_directories(path / "pending");
    auto file_path = path / "pending" / name;
    // NOLINTNEXTLINE(misc-include-cleaner)
    std::ofstream ofs(file_path, std::ios::binary);
    std::vector<char> zeros(size_bytes, 0);
    ofs.write(zeros.data(), static_cast<std::streamsize>(size_bytes));
    return file_path;
  }

  // Create a .meta sidecar inside pending/ (Crashpad writes one per .dmp).
  // NOLINTNEXTLINE(modernize-use-nodiscard)
  std::filesystem::path CreateMetaInPending(const std::string& name,
                                            size_t size_bytes = kBytes64) const {
    std::filesystem::create_directories(path / "pending");
    auto file_path = path / "pending" / name;
    // NOLINTNEXTLINE(misc-include-cleaner)
    std::ofstream ofs(file_path, std::ios::binary);
    std::vector<char> zeros(size_bytes, 0);
    ofs.write(zeros.data(), static_cast<std::streamsize>(size_bytes));
    return file_path;
  }

  // Create a .crashdump file of the given size (filled with zeros). Returns the path.
  // NOLINTNEXTLINE(modernize-use-nodiscard)
  std::filesystem::path CreateCrashdump(const std::string& name,
                                        size_t size_bytes = kBytes1K) const {
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
  tmp.CreateDmpInPending("a.dmp", kBytes4K);
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, kBytes4K);
}

TEST(DiskManagerTest, TotalSizeMultipleFiles) {
  const TempDir tmp;
  tmp.CreateDmpInPending("a.dmp", kBytes1K);
  tmp.CreateDmpInPending("b.dmp", kBytes2K);
  tmp.CreateDmpInPending("c.dmp", kBytes512);
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, kBytes1K + kBytes2K + kBytes512);
}

TEST(DiskManagerTest, TotalSizeIgnoresNonDmpFiles) {
  const TempDir tmp;
  tmp.CreateDmpInPending("a.dmp", kBytes1K);
  // Create a non-.dmp file alongside it.
  std::filesystem::create_directories(tmp.path / "pending");
  std::ofstream(tmp.path / "pending" / "notes.txt") << "hello";
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, kBytes1K);
}

// pending/ does not exist yet (no crashes recorded) — should return 0, not an error.
TEST(DiskManagerTest, TotalSizeNoPendingDir) {
  const TempDir tmp;
  auto size_or = GetTotalMinidumpSize(tmp.path.string());
  ASSERT_TRUE(size_or.ok()) << size_or.status();
  EXPECT_EQ(*size_or, 0U);
}

// ── PruneMinidumps ────────────────────────────────────────────────────────────

TEST(DiskManagerTest, NoLimitsDoesNothing) {
  const TempDir tmp;
  tmp.CreateDmpInPending("a.dmp", kBytes1K);
  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = 0;
  cfg.max_age_seconds = 0;
  EXPECT_TRUE(PruneMinidumps(cfg).ok());
  // File still present.
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "pending" / "a.dmp"));
}

TEST(DiskManagerTest, SizeLimitDeletesOldestFirst) {
  const TempDir tmp;
  // Create files with different mtimes so oldest is deterministic.
  auto old = tmp.CreateDmpInPending("old.dmp", kBytes1K);
  auto newer = tmp.CreateDmpInPending("newer.dmp", kBytes1K);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old, now - kAge100s);
  TempDir::SetMtime(newer, now - kAge10s);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1K;  // allow only one file
  cfg.max_age_seconds = 0;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());

  // old.dmp should be deleted; newer.dmp should survive.
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "pending" / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "pending" / "newer.dmp"));
}

TEST(DiskManagerTest, SizeLimitUnderBudgetDoesNothing) {
  const TempDir tmp;
  tmp.CreateDmpInPending("a.dmp", kBytes512);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1M;  // 1 MB — well above 512 bytes
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "pending" / "a.dmp"));
}

TEST(DiskManagerTest, AgeLimit) {
  const TempDir tmp;
  auto old = tmp.CreateDmpInPending("old.dmp", kBytes512);
  auto fresh = tmp.CreateDmpInPending("fresh.dmp", kBytes512);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old, now - kAge1000s);  // 1000 seconds ago
  TempDir::SetMtime(fresh, now - kAge10s);  // 10 seconds ago

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_age_seconds = kAge500s;  // delete anything older than 500s

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "pending" / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "pending" / "fresh.dmp"));
}

// pending/ does not exist yet — PruneMinidumps must succeed silently.
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
  tmp.CreateDmpInPending("a.dmp", kBytes1K);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_bytes = kBytes1K;  // exact — should not prune
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(tmp.path / "pending" / "a.dmp"));
}

// ── PruneMinidumps: export_path ───────────────────────────────────────────────

// Empty export_path with active limits: no error, db_path still pruned normally.
TEST(DiskManagerTest, ExportPathEmptyIsSkipped) {
  const TempDir dbt;
  auto old = dbt.CreateDmpInPending("old.dmp", kBytes1K);
  auto newer = dbt.CreateDmpInPending("newer.dmp", kBytes1K);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old, now - kAge100s);
  TempDir::SetMtime(newer, now - kAge10s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = "";      // explicitly empty — no export pruning
  cfg.max_bytes = kBytes1K;  // would prune one file
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  // db_path/pending is still pruned as normal
  EXPECT_FALSE(std::filesystem::exists(dbt.path / "pending" / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(dbt.path / "pending" / "newer.dmp"));
}

// Non-empty export_path that does not exist returns an error (with limits active).
TEST(DiskManagerTest, ExportPathNonexistentReturnsError) {
  const TempDir dbt;
  dbt.CreateDmp("a.dmp", kBytes1K);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = "/nonexistent/export/path/that/does/not/exist";
  cfg.max_bytes = kBytes1K;
  EXPECT_FALSE(PruneMinidumps(cfg).ok());
}

// .crashdump files in export_path are pruned when total size exceeds max_bytes.
TEST(DiskManagerTest, ExportPathSizeLimitDeletesOldestFirst) {
  const TempDir dbt;
  const TempDir exp;
  dbt.CreateDmpInPending("a.dmp", kBytes512);  // db_path under budget — should not be touched

  auto old_cd = exp.CreateCrashdump("old.crashdump", kBytes1K);
  auto new_cd = exp.CreateCrashdump("new.crashdump", kBytes1K);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old_cd, now - kAge100s);
  TempDir::SetMtime(new_cd, now - kAge10s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = exp.path.string();
  cfg.max_bytes = kBytes1K;  // allow only one .crashdump

  ASSERT_TRUE(PruneMinidumps(cfg).ok());

  EXPECT_FALSE(std::filesystem::exists(exp.path / "old.crashdump"));
  EXPECT_TRUE(std::filesystem::exists(exp.path / "new.crashdump"));
  // db_path/pending untouched
  EXPECT_TRUE(std::filesystem::exists(dbt.path / "pending" / "a.dmp"));
}

// .crashdump files in export_path are pruned when older than max_age_seconds.
TEST(DiskManagerTest, ExportPathAgeLimitDeletesStaleFiles) {
  const TempDir dbt;
  const TempDir exp;

  auto old_cd = exp.CreateCrashdump("old.crashdump", kBytes512);
  auto fresh_cd = exp.CreateCrashdump("fresh.crashdump", kBytes512);
  const time_t now = time(nullptr);
  TempDir::SetMtime(old_cd, now - kAge1000s);  // 1000 s ago — over limit
  TempDir::SetMtime(fresh_cd, now - kAge10s);  // 10 s ago — under limit

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = exp.path.string();
  cfg.max_age_seconds = kAge500s;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_FALSE(std::filesystem::exists(exp.path / "old.crashdump"));
  EXPECT_TRUE(std::filesystem::exists(exp.path / "fresh.crashdump"));
}

// .crashdump file within limits is preserved.
TEST(DiskManagerTest, ExportPathUnderBudgetPreservesFiles) {
  const TempDir dbt;
  const TempDir exp;
  exp.CreateCrashdump("a.crashdump", kBytes512);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = exp.path.string();
  cfg.max_bytes = kBytes1M;  // well above 512 bytes
  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_TRUE(std::filesystem::exists(exp.path / "a.crashdump"));
}

// .dmp and .crashdump files are pruned independently: a .crashdump file in
// export_path is not counted toward db_path's budget and vice versa.
TEST(DiskManagerTest, BothPathsPrunedIndependentlyByExtension) {
  const TempDir dbt;
  const TempDir exp;

  // db_path: two .dmp files in pending/, total 2 KB
  auto old_dmp = dbt.CreateDmpInPending("old.dmp", kBytes1K);
  auto new_dmp = dbt.CreateDmpInPending("new.dmp", kBytes1K);
  // export_path: two .crashdump files, total 2 KB
  auto old_cd = exp.CreateCrashdump("old.crashdump", kBytes1K);
  auto new_cd = exp.CreateCrashdump("new.crashdump", kBytes1K);

  const time_t now = time(nullptr);
  TempDir::SetMtime(old_dmp, now - kAge100s);
  TempDir::SetMtime(new_dmp, now - kAge10s);
  TempDir::SetMtime(old_cd, now - kAge100s);
  TempDir::SetMtime(new_cd, now - kAge10s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = exp.path.string();
  cfg.max_bytes = kBytes1K;  // allow only one file in each directory

  ASSERT_TRUE(PruneMinidumps(cfg).ok());

  // oldest .dmp pruned from db_path/pending
  EXPECT_FALSE(std::filesystem::exists(dbt.path / "pending" / "old.dmp"));
  EXPECT_TRUE(std::filesystem::exists(dbt.path / "pending" / "new.dmp"));
  // oldest .crashdump pruned from export_path
  EXPECT_FALSE(std::filesystem::exists(exp.path / "old.crashdump"));
  EXPECT_TRUE(std::filesystem::exists(exp.path / "new.crashdump"));
}

// ── .meta sidecar cleanup ─────────────────────────────────────────────────────

// Pruning a .dmp file must also delete the .meta sidecar written by Crashpad.
TEST(DiskManagerTest, MetaSidecarDeletedWithDmp) {
  const TempDir tmp;
  auto dmp = tmp.CreateDmpInPending("crash.dmp", kBytes1K);
  auto meta = tmp.CreateMetaInPending("crash.meta", kBytes64);

  const time_t now = time(nullptr);
  TempDir::SetMtime(dmp, now - kAge1000s);
  TempDir::SetMtime(meta, now - kAge1000s);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_age_seconds = kAge500s;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "pending" / "crash.dmp"));
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "pending" / "crash.meta"));
}

// Pruning a .dmp that has no .meta sidecar must still succeed.
TEST(DiskManagerTest, MissingMetaSidecarIsNotAnError) {
  const TempDir tmp;
  auto dmp = tmp.CreateDmpInPending("crash.dmp", kBytes1K);
  const time_t now = time(nullptr);
  TempDir::SetMtime(dmp, now - kAge1000s);

  DiskManagerConfig cfg;
  cfg.db_path = tmp.path.string();
  cfg.max_age_seconds = kAge500s;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  EXPECT_FALSE(std::filesystem::exists(tmp.path / "pending" / "crash.dmp"));
}

// .dmp files in export_path are not pruned (wrong extension for that directory).
TEST(DiskManagerTest, ExportPathIgnoresDmpFiles) {
  const TempDir dbt;
  const TempDir exp;
  // Put a .dmp file directly in export_path — it should NOT be removed.
  auto dmp_in_exp = exp.CreateDmp("stray.dmp", kBytes1K);
  exp.CreateCrashdump("real.crashdump", kBytes1K);

  const time_t now = time(nullptr);
  TempDir::SetMtime(dmp_in_exp, now - kAge1000s);  // very old — would be pruned if scanned
  TempDir::SetMtime(exp.path / "real.crashdump", now - kAge1000s);

  DiskManagerConfig cfg;
  cfg.db_path = dbt.path.string();
  cfg.export_path = exp.path.string();
  cfg.max_bytes = kBytes512;  // tight limit to force pruning
  cfg.max_age_seconds = kAge10s;

  ASSERT_TRUE(PruneMinidumps(cfg).ok());
  // .crashdump was pruned because it's old
  EXPECT_FALSE(std::filesystem::exists(exp.path / "real.crashdump"));
  // .dmp in export_path was not touched
  EXPECT_TRUE(std::filesystem::exists(exp.path / "stray.dmp"));
}

}  // namespace
}  // namespace crashomon
