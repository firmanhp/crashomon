// daemon/disk_manager.h — Minidump database disk space management
//
// Prunes old minidump files from the database directory to stay within
// configurable size and age limits. Deletes oldest files first.
//
// This header is internal to crashomon-watcherd.
// absl::Status/absl::StatusOr are safe here — this is not a public API.

#pragma once

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace crashomon {

struct DiskManagerConfig {
  std::string db_path;
  std::string export_path;       // export directory to prune alongside db_path; empty = disabled
  uint64_t max_bytes = 0;        // total size limit; 0 = unlimited
  uint32_t max_age_seconds = 0;  // per-file age limit; 0 = unlimited
};

// Delete minidump files in db_path that exceed max_bytes or max_age_seconds.
// Files are deleted in oldest-first order. Only files with the ".dmp"
// extension are considered. Returns OK even if no pruning was needed.
absl::Status PruneMinidumps(const DiskManagerConfig& config);

// Sum the sizes of all ".dmp" files in db_path.
// Returns an error if the directory cannot be listed.
absl::StatusOr<uint64_t> GetTotalMinidumpSize(const std::string& db_path);

}  // namespace crashomon
