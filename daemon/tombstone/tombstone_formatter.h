// tombstone/tombstone_formatter.h — Android-style tombstone formatter
//
// Formats a MinidumpInfo into a multi-line tombstone string modelled after
// Android's debuggerd output. The caller writes the result to journald / stderr.

#pragma once

#include <string>

#include "daemon/tombstone/minidump_reader.h"

namespace crashomon {

// Format a MinidumpInfo into an Android-style tombstone string.
// The output begins and ends with the *** separator line.
// Only the crashing thread (registers + backtrace) is included; other threads
// remain in the minidump but are not printed.
std::string FormatTombstone(const MinidumpInfo& info);

}  // namespace crashomon
