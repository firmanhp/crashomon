// daemon/real_tombstone.cpp — production ITombstone implementation.
// Not compiled into the test binary; tests use MockTombstone instead.

#include "daemon/tombstone/minidump_reader.h"
#include "daemon/tombstone/tombstone_formatter.h"
#include "daemon/worker.h"

namespace crashomon {

absl::StatusOr<MinidumpInfo> RealTombstone::ReadMinidump(const std::string& path) {
  return crashomon::ReadMinidump(path);
}

std::string RealTombstone::FormatTombstone(const MinidumpInfo& info) {
  return crashomon::FormatTombstone(info);
}

}  // namespace crashomon
