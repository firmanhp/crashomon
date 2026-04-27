// daemon/worker.h — worker thread state and minidump processing
//
// Extracted from main.cpp so the processing logic can be unit-tested without
// linking the full watcherd binary.
//
// ProcessNewMinidump is the unit of correctness: it reads one minidump, logs
// a tombstone, optionally exports a .crashdump copy, and then unconditionally
// prunes the database.  The prune must run on every path — including the error
// path — so that a full disk (which causes Crashpad to write a partial,
// unreadable dump) does not prevent space from being reclaimed.

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>

#include "absl/status/statusor.h"
#include "daemon/disk_manager.h"
#include "daemon/tombstone/minidump_reader.h"

namespace crashomon {

// Tombstone operations needed by the worker: read a minidump file and format
// a tombstone string from the parsed result.  Injected so tests can mock
// without linking crashomon_tombstone or breakpad_processor.
class ITombstone {
 public:
  ITombstone() = default;
  virtual ~ITombstone() = default;
  ITombstone(const ITombstone&) = delete;
  ITombstone& operator=(const ITombstone&) = delete;
  ITombstone(ITombstone&&) = delete;
  ITombstone& operator=(ITombstone&&) = delete;

  virtual absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path) = 0;
  virtual std::string FormatTombstone(const MinidumpInfo& info) = 0;
};

// Production implementation backed by the real tombstone library.
class RealTombstone : public ITombstone {
 public:
  absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path) override;
  std::string FormatTombstone(const MinidumpInfo& info) override;
};

// Shared state for the minidump processing worker thread.
// `pending`, `cv`, and `stop` are shared between the poll loop and the worker,
// protected by `mu`.  `rate_limit_map` is accessed only from the worker thread.
struct WorkerState {
  std::queue<std::string> pending;
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
  // Keyed by "process_name:signal_info:fault_addr_hex"; value is last-seen time.
  // Single-threaded access (worker only) — no additional lock needed.
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> rate_limit_map;
};

// Process one minidump file: read, log tombstone, export if configured, prune.
// Pruning is unconditional — it runs even when the dump is unreadable, so a
// full disk that produces partial writes doesn't wedge the pruning cycle.
// When patch_build_ids is true, PatchMissingBuildIds() is called first so
// modules without .note.gnu.build-id sections get a matching fallback ID.
void ProcessNewMinidump(const std::string& path, WorkerState& state,
                        const DiskManagerConfig& prune_cfg, std::string_view export_path,
                        ITombstone& tombstone, bool patch_build_ids = true);

// Worker thread entry point: dequeues and processes minidumps from `state`
// until state.stop is true and the queue is empty.
void RunWorker(WorkerState& state, const DiskManagerConfig& prune_cfg, std::string_view export_path,
               ITombstone& tombstone, bool patch_build_ids = true);

}  // namespace crashomon
