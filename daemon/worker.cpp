// daemon/worker.cpp — minidump processing worker

#include "daemon/worker.h"

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include "daemon/disk_manager.h"
#include "tombstone/minidump_reader.h"
#include "tombstone/tombstone_formatter.h"

namespace {

// Suppress identical crash signatures within this window to limit log/disk spam.
constexpr std::chrono::seconds kRateLimitWindow{30};
// Size of the hex buffer for a uint64_t address (16 hex digits + null terminator).
constexpr size_t kFaultAddrHexBufSize = 17;
// Radix for fault address formatting.
constexpr int kHexBase = 16;

void Log(std::string_view msg) {
  std::fwrite(msg.data(), 1, msg.size(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

void LogTombstone(std::string_view msg) {
  std::fwrite(msg.data(), 1, msg.size(), stdout);
  std::fflush(stdout);
}

// Copy src_path into export_path as {process}_{build_id8}_{YYYYMMDDHHmmss}.crashdump.
// Logs and continues on failure — export is best-effort.
void ExportMinidump(const std::string& src_path, std::string_view export_path,
                    const crashomon::MinidumpInfo& info) {
  constexpr std::size_t build_id_len = 8;
  constexpr std::size_t timestamp_buf_len = 15;  // "YYYYMMDDHHmmSS" + '\0'

  std::string name;
  for (const char chr : info.process_name) {
    name += (std::isalnum(static_cast<unsigned char>(chr)) != 0 || chr == '-') ? chr : '_';
  }

  const std::string_view raw_id =
      info.modules.empty() ? "" : std::string_view(info.modules[0].build_id);
  std::string bid;
  for (const char chr : raw_id) {
    if (std::isxdigit(static_cast<unsigned char>(chr)) != 0) {
      bid += static_cast<char>(std::tolower(static_cast<unsigned char>(chr)));
    }
    if (bid.size() == build_id_len) {
      break;
    }
  }
  if (bid.size() < build_id_len) {
    bid.resize(build_id_len, '0');
  }

  const std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  gmtime_r(&now, &tm_buf);
  std::array<char, timestamp_buf_len> timestamp{};
  std::strftime(timestamp.data(), timestamp.size(), "%Y%m%d%H%M%S", &tm_buf);

  const std::filesystem::path dst =
      std::filesystem::path(std::string(export_path)) /
      (name + "_" + bid + "_" + timestamp.data() + ".crashdump");
  std::error_code err;
  std::filesystem::copy_file(src_path, dst, std::filesystem::copy_options::overwrite_existing, err);
  if (err) {
    Log(std::string{"crashomon-watcherd: export failed \xe2\x86\x92 "} + dst.string() + ": " +
        err.message());
  }
}

}  // namespace

namespace crashomon {

void ProcessNewMinidump(const std::string& path, WorkerState& state,
                        const DiskManagerConfig& prune_cfg,
                        std::string_view export_path) {
  auto info_or = ReadMinidump(path);
  if (!info_or.ok()) {
    Log(std::string{"crashomon-watcherd: failed to read minidump '"} + path +
        "': " + std::string(info_or.status().message()));
  } else {
    const auto& info = *info_or;

    // Rate limiting: suppress identical crash signatures within kRateLimitWindow.
    // Build the key using to_chars (non-variadic, no printf dependency).
    std::array<char, kFaultAddrHexBufSize> fault_hex{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::to_chars(fault_hex.data(), fault_hex.data() + fault_hex.size() - 1, info.fault_addr,
                  kHexBase);
    const std::string key = info.process_name + ":" + info.signal_info + ":" + fault_hex.data();

    const auto now = std::chrono::steady_clock::now();
    const auto rate_it = state.rate_limit_map.find(key);
    if (rate_it != state.rate_limit_map.end() && (now - rate_it->second) < kRateLimitWindow) {
      Log(std::string{"crashomon-watcherd: suppressed duplicate crash ("} + key + ")");
    } else {
      state.rate_limit_map[key] = now;
      LogTombstone(FormatTombstone(info));
      if (!export_path.empty()) {
        ExportMinidump(path, export_path, info);
      }
    }
  }

  if (const auto prune_status = PruneMinidumps(prune_cfg); !prune_status.ok()) {
    Log(std::string{"crashomon-watcherd: prune failed: "} + std::string(prune_status.message()));
  }
}

void RunWorker(WorkerState& state, const DiskManagerConfig& prune_cfg,
               std::string_view export_path) {
  while (true) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(state.mu);
      state.cv.wait(lock, [&state] { return state.stop || !state.pending.empty(); });
      if (state.stop && state.pending.empty()) {
        return;
      }
      path = std::move(state.pending.front());
      state.pending.pop();
    }
    // No lock held during processing — allows the poll loop to keep enqueuing.
    ProcessNewMinidump(path, state, prune_cfg, export_path);
  }
}

}  // namespace crashomon
