// daemon/disk_manager.cpp — Minidump database disk space management

#include "daemon/disk_manager.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <ranges>  // std::ranges::sort is in <algorithm> in C++20, but <ranges> is required on some implementations; keep for portability.
#include <string>
#include <system_error>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "spdlog/spdlog.h"

namespace crashomon {
namespace {

// A single Crashpad crash report: a .dmp file paired with its .meta sidecar.
// The .meta file may not exist on disk if Crashpad did not write it.
struct CrashReport {
  std::filesystem::path dmp_path;
  std::filesystem::path meta_path;  // <same stem>.meta
  uint64_t size = 0;
  time_t mtime = 0;  // seconds since epoch
};

// A single-file export entry (e.g. .crashdump).
struct ExportFile {
  std::filesystem::path path;
  uint64_t size = 0;
  time_t mtime = 0;  // seconds since epoch
};

// List all .dmp files in dir as CrashReport entries, sorted oldest-first.
// The meta_path is always populated (as <stem>.meta) regardless of whether
// the file exists on disk — removal is best-effort.
absl::StatusOr<std::vector<CrashReport>> ListCrashReports(const std::string& dir) {
  std::error_code err;
  if (!std::filesystem::is_directory(dir, err) || err) {
    return absl::NotFoundError(absl::StrCat("Not a directory: ", dir));
  }

  std::vector<CrashReport> reports;
  for (const auto& entry : std::filesystem::directory_iterator(dir, err)) {
    if (err) {
      break;
    }
    if (!entry.is_regular_file(err) || err) {
      continue;
    }
    if (entry.path().extension() != ".dmp") {
      continue;
    }

    struct stat file_stat {};
    if (::stat(entry.path().c_str(), &file_stat) != 0) {
      continue;
    }

    auto meta = entry.path();
    meta.replace_extension(".meta");
    reports.push_back(CrashReport{.dmp_path = entry.path(),
                                  .meta_path = std::move(meta),
                                  .size = static_cast<uint64_t>(file_stat.st_size),
                                  .mtime = file_stat.st_mtime});
  }

  if (err) {
    return absl::InternalError(
        absl::StrCat("directory_iterator error for ", dir, ": ", err.message()));
  }

  std::ranges::sort(reports, {}, &CrashReport::mtime);
  return reports;
}

// List all files with the given extension in dir, sorted oldest-first.
absl::StatusOr<std::vector<ExportFile>> ListExportFiles(const std::string& dir,
                                                        std::string_view ext) {
  std::error_code err;
  if (!std::filesystem::is_directory(dir, err) || err) {
    return absl::NotFoundError(absl::StrCat("Not a directory: ", dir));
  }

  std::vector<ExportFile> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir, err)) {
    if (err) {
      break;
    }
    if (!entry.is_regular_file(err) || err) {
      continue;
    }
    if (entry.path().extension() != ext) {
      continue;
    }

    struct stat file_stat {};
    if (::stat(entry.path().c_str(), &file_stat) != 0) {
      continue;
    }

    files.push_back(ExportFile{.path = entry.path(),
                               .size = static_cast<uint64_t>(file_stat.st_size),
                               .mtime = file_stat.st_mtime});
  }

  if (err) {
    return absl::InternalError(
        absl::StrCat("directory_iterator error for ", dir, ": ", err.message()));
  }

  std::ranges::sort(files, {}, &ExportFile::mtime);
  return files;
}

// Remove a crash report (the .dmp and its .meta sidecar).
// Returns an error if the .dmp cannot be removed. .meta removal is best-effort.
absl::Status RemoveCrashReport(const CrashReport& report, uint64_t& total_bytes,
                               const char* reason) {
  std::error_code err;
  if (!std::filesystem::remove(report.dmp_path, err) || err) {
    return absl::InternalError(
        absl::StrCat("Failed to remove ", report.dmp_path.string(), ": ", err.message()));
  }
  total_bytes -= report.size;
  spdlog::info("crashomon-watcherd: pruned {} ({} bytes, reason: {})", report.dmp_path.string(),
               report.size, reason);

  std::error_code meta_err;
  std::filesystem::remove(report.meta_path, meta_err);
  return absl::OkStatus();
}

// Prune Crashpad pending/ directory entries (each report is a .dmp + .meta pair).
// Files are deleted oldest-first until total size is within max_bytes and all
// files are within max_age_seconds. No-op if both limits are zero.
absl::Status PrunePendingDir(const std::string& dir, uint64_t max_bytes, uint32_t max_age_seconds) {
  if (max_bytes == 0 && max_age_seconds == 0) {
    return absl::OkStatus();
  }

  auto reports_or = ListCrashReports(dir);
  if (!reports_or.ok()) {
    return reports_or.status();
  }
  auto& reports = *reports_or;

  const time_t now = ::time(nullptr);

  uint64_t total_bytes = 0;
  for (const auto& report : reports) {
    total_bytes += report.size;
  }

  for (const auto& report : reports) {
    const bool over_size = (max_bytes > 0 && total_bytes > max_bytes);
    const bool over_age = (max_age_seconds > 0 && now > report.mtime &&
                           static_cast<uint64_t>(now - report.mtime) > max_age_seconds);
    if (!over_size && !over_age) {
      continue;
    }
    if (auto status = RemoveCrashReport(report, total_bytes, over_age ? "age" : "size");
        !status.ok()) {
      return status;
    }
  }

  return absl::OkStatus();
}

// Prune a flat export directory of single-file entries (e.g. .crashdump).
// Files are deleted oldest-first until total size is within max_bytes and all
// files are within max_age_seconds. No-op if both limits are zero.
absl::Status PruneExportDir(const std::string& dir, std::string_view ext, uint64_t max_bytes,
                            uint32_t max_age_seconds) {
  if (max_bytes == 0 && max_age_seconds == 0) {
    return absl::OkStatus();
  }

  auto files_or = ListExportFiles(dir, ext);
  if (!files_or.ok()) {
    return files_or.status();
  }
  auto& files = *files_or;

  const time_t now = ::time(nullptr);

  uint64_t total_bytes = 0;
  for (const auto& file : files) {
    total_bytes += file.size;
  }

  for (const auto& file : files) {
    const bool over_size = (max_bytes > 0 && total_bytes > max_bytes);
    const bool over_age = (max_age_seconds > 0 && now > file.mtime &&
                           static_cast<uint64_t>(now - file.mtime) > max_age_seconds);
    if (!over_size && !over_age) {
      continue;
    }

    std::error_code err;
    if (!std::filesystem::remove(file.path, err) || err) {
      return absl::InternalError(
          absl::StrCat("Failed to remove ", file.path.string(), ": ", err.message()));
    }
    total_bytes -= file.size;
    const char* reason = over_age ? "age" : "size";
    spdlog::info("crashomon-watcherd: pruned {} ({} bytes, reason: {})", file.path.string(),
                 file.size, reason);
  }

  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<uint64_t> GetTotalMinidumpSize(const std::string& db_path) {
  // Crashpad writes minidumps to db_path/pending/ after the write is complete.
  const std::string pending_dir = db_path + "/pending";
  auto reports_or = ListCrashReports(pending_dir);
  if (!reports_or.ok()) {
    // pending/ may not exist yet if no crashes have been recorded — treat as 0.
    return static_cast<uint64_t>(0);
  }

  uint64_t total = 0;
  for (const auto& report : *reports_or) {
    total += report.size;
  }
  return total;
}

absl::Status PruneMinidumps(const DiskManagerConfig& config) {
  // Crashpad writes completed minidumps into db_path/pending/; prune there.
  const std::string pending_dir = config.db_path + "/pending";
  if (auto status = PrunePendingDir(pending_dir, config.max_bytes, config.max_age_seconds);
      !status.ok()) {
    // pending/ may not exist yet (no crashes recorded) — not an error.
    if (status.code() != absl::StatusCode::kNotFound) {
      return status;
    }
  }
  if (!config.export_path.empty()) {
    return PruneExportDir(config.export_path, ".crashdump", config.max_bytes,
                          config.max_age_seconds);
  }
  return absl::OkStatus();
}

}  // namespace crashomon
