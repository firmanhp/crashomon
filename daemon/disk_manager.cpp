// daemon/disk_manager.cpp — Minidump database disk space management

#include "daemon/disk_manager.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <ranges>  // NOLINT(misc-include-cleaner) — std::ranges::sort is in <algorithm> in C++20, but <ranges> is required on some implementations; keep for portability.
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

// List all .dmp files in db_path, sorted oldest-first by modification time.
// Returns an error if the directory cannot be opened.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
absl::StatusOr<std::vector<DmpFile>> ListDmpFiles(const std::string& db_path) {
  std::error_code err;
  if (!std::filesystem::is_directory(db_path, err) || err) {
    return absl::NotFoundError(std::string("Not a directory: ") + db_path);
  }

  std::vector<DmpFile> files;
  for (const auto& entry : std::filesystem::directory_iterator(db_path, err)) {
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

    files.push_back(DmpFile{.path = entry.path(),
                            .size = static_cast<uint64_t>(file_stat.st_size),
                            .mtime = file_stat.st_mtime});
  }

  if (err) {
    return absl::InternalError(std::string("directory_iterator error for ") + db_path + ": " +
                               err.message());
  }

  std::ranges::sort(files, {}, &DmpFile::mtime);
  return files;
}

}  // namespace

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
absl::StatusOr<uint64_t> GetTotalMinidumpSize(const std::string& db_path) {
  auto files_or = ListDmpFiles(db_path);
  if (!files_or.ok()) {
    return files_or.status();
  }

  uint64_t total = 0;
  for (const auto& dmp : *files_or) {
    total += dmp.size;
  }
  return total;
}

// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
absl::Status PruneMinidumps(const DiskManagerConfig& config) {
  if (config.max_bytes == 0 && config.max_age_seconds == 0) {
    return absl::OkStatus();
  }

  auto files_or = ListDmpFiles(config.db_path);
  if (!files_or.ok()) {
    return files_or.status();
  }
  auto& files = *files_or;

  const time_t now = ::time(nullptr);

  uint64_t total_bytes = 0;
  for (const auto& dmp : files) {
    total_bytes += dmp.size;
  }

  for (const auto& dmp : files) {
    const bool over_size = (config.max_bytes > 0 && total_bytes > config.max_bytes);
    const bool over_age = (config.max_age_seconds > 0 && now > dmp.mtime &&
                           static_cast<uint64_t>(now - dmp.mtime) > config.max_age_seconds);

    if (!over_size && !over_age) {
      continue;
    }

    std::error_code err;
    if (!std::filesystem::remove(dmp.path, err) || err) {
      return absl::InternalError(std::string("Failed to remove ") + dmp.path.string() + ": " +
                                 err.message());
    }
    total_bytes -= dmp.size;
  }

  return absl::OkStatus();
}

}  // namespace crashomon
