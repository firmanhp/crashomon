// daemon/disk_manager.cpp — Minidump database disk space management

#include "daemon/disk_manager.h"

#include <sys/stat.h>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace crashomon {
namespace {

struct DmpFile {
  std::filesystem::path path;
  uint64_t size = 0;
  time_t mtime = 0; // seconds since epoch
};

// List all .dmp files in db_path, sorted oldest-first by modification time.
// Returns an error if the directory cannot be opened.
absl::StatusOr<std::vector<DmpFile>> ListDmpFiles(
    const std::string& db_path) {
  std::error_code ec;
  if (!std::filesystem::is_directory(db_path, ec) || ec) {
    return absl::NotFoundError(
        std::string("Not a directory: ") + db_path);
  }

  std::vector<DmpFile> files;
  for (const auto& entry :
       std::filesystem::directory_iterator(db_path, ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec) || ec) continue;
    if (entry.path().extension() != ".dmp") continue;

    struct stat st;
    if (::stat(entry.path().c_str(), &st) != 0) continue;

    DmpFile f;
    f.path = entry.path();
    f.size = static_cast<uint64_t>(st.st_size);
    f.mtime = st.st_mtime;
    files.push_back(std::move(f));
  }

  if (ec) {
    return absl::InternalError(
        std::string("directory_iterator error for ") + db_path +
        ": " + ec.message());
  }

  std::sort(files.begin(), files.end(),
            [](const DmpFile& a, const DmpFile& b) {
              return a.mtime < b.mtime;
            });
  return files;
}

}  // namespace

absl::StatusOr<uint64_t> GetTotalMinidumpSize(const std::string& db_path) {
  auto files_or = ListDmpFiles(db_path);
  if (!files_or.ok()) return files_or.status();

  uint64_t total = 0;
  for (const auto& f : *files_or) total += f.size;
  return total;
}

absl::Status PruneMinidumps(const DiskManagerConfig& config) {
  if (config.max_bytes == 0 && config.max_age_seconds == 0) {
    return absl::OkStatus();
  }

  auto files_or = ListDmpFiles(config.db_path);
  if (!files_or.ok()) return files_or.status();
  auto& files = *files_or;

  time_t now = ::time(nullptr);

  uint64_t total_bytes = 0;
  for (const auto& f : files) total_bytes += f.size;

  for (const auto& f : files) {
    bool over_size = (config.max_bytes > 0 && total_bytes > config.max_bytes);
    bool over_age = (config.max_age_seconds > 0 && now > f.mtime &&
                     static_cast<uint64_t>(now - f.mtime) > config.max_age_seconds);

    if (!over_size && !over_age) continue;

    std::error_code ec;
    if (!std::filesystem::remove(f.path, ec) || ec) {
      return absl::InternalError(
          std::string("Failed to remove ") + f.path.string() +
          ": " + ec.message());
    }
    total_bytes -= f.size;
  }

  return absl::OkStatus();
}

}  // namespace crashomon
