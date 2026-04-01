// tombstone/tombstone_formatter.h — Android-style tombstone formatter
//
// Formats a MinidumpInfo into a multi-line tombstone string modelled after
// Android's debuggerd output. The caller writes the result to journald / stderr.

#pragma once

#include <string>

#include "tombstone/minidump_reader.h"

namespace crashomon {

// Format a MinidumpInfo into an Android-style tombstone string.
// The output begins and ends with the *** separator line.
// Registers are shown only for the crashing thread.
// Other threads are appended with a "--- --- --- thread TID --- --- ---" header.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::string FormatTombstone(const MinidumpInfo& info);

}  // namespace crashomon
