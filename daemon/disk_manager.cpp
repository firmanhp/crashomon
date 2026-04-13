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

namespace crashomon {
namespace {

struct DmpFile {
  std::filesystem::path path;
  uint64_t size = 0;
  time_t mtime = 0;  // seconds since epoch
};

// List all files with the given extension in dir, sorted oldest-first by mtime.
// Returns an error if the directory cannot be opened.
absl::StatusOr<std::vector<DmpFile>> ListFilesByExtension(const std::string& dir,
                                                          std::string_view ext) {
  std::error_code err;
  if (!std::filesystem::is_directory(dir, err) || err) {
    return absl::NotFoundError(std::string("Not a directory: ") + dir);
  }

  std::vector<DmpFile> files;
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

    files.push_back(DmpFile{.path = entry.path(),
                            .size = static_cast<uint64_t>(file_stat.st_size),
                            .mtime = file_stat.st_mtime});
  }

  if (err) {
    return absl::InternalError(std::string("directory_iterator error for ") + dir + ": " +
                               err.message());
  }

  std::ranges::sort(files, {}, &DmpFile::mtime);
  return files;
}

// Prune files from dir (oldest-first) until total size is within max_bytes and
// all files are within max_age_seconds.  No-ops if both limits are zero.
absl::Status PruneDirectory(const std::string& dir, std::string_view ext,
                             uint64_t max_bytes, uint32_t max_age_seconds) {
  if (max_bytes == 0 && max_age_seconds == 0) {
    return absl::OkStatus();
  }

  auto files_or = ListFilesByExtension(dir, ext);
  if (!files_or.ok()) {
    return files_or.status();
  }
  auto& files = *files_or;

  const time_t now = ::time(nullptr);

  uint64_t total_bytes = 0;
  for (const auto& f : files) {
    total_bytes += f.size;
  }

  for (const auto& f : files) {
    const bool over_size = (max_bytes > 0 && total_bytes > max_bytes);
    const bool over_age = (max_age_seconds > 0 && now > f.mtime &&
                           static_cast<uint64_t>(now - f.mtime) > max_age_seconds);

    if (!over_size && !over_age) {
      continue;
    }

    std::error_code err;
    if (!std::filesystem::remove(f.path, err) || err) {
      return absl::InternalError(std::string("Failed to remove ") + f.path.string() + ": " +
                                 err.message());
    }
    total_bytes -= f.size;
  }

  return absl::OkStatus();
}

}  // namespace

absl::StatusOr<uint64_t> GetTotalMinidumpSize(const std::string& db_path) {
  auto files_or = ListFilesByExtension(db_path, ".dmp");
  if (!files_or.ok()) {
    return files_or.status();
  }

  uint64_t total = 0;
  for (const auto& dmp : *files_or) {
    total += dmp.size;
  }
  return total;
}

absl::Status PruneMinidumps(const DiskManagerConfig& config) {
  if (auto s = PruneDirectory(config.db_path, ".dmp", config.max_bytes, config.max_age_seconds);
      !s.ok()) {
    return s;
  }
  if (!config.export_path.empty()) {
    if (auto s = PruneDirectory(config.export_path, ".crashdump", config.max_bytes,
                                config.max_age_seconds);
        !s.ok()) {
      return s;
    }
  }
  return absl::OkStatus();
}

}  // namespace crashomon
