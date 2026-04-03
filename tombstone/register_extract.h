// tombstone/register_extract.h — internal register extraction helpers.
// NOT a public header — consumers of crashomon do not include this.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Processor headers pull in minidump_format.h → breakpad_types.h (defines
// uint128_struct). Include them BEFORE the CPU-specific headers.
#include "google_breakpad/processor/minidump.h"  // IWYU pragma: keep
#include "google_breakpad/common/minidump_cpu_amd64.h"
#include "google_breakpad/common/minidump_cpu_arm64.h"

namespace crashomon {

// Collect AMD64 GPRs in tombstone display order.
std::vector<std::pair<std::string, uint64_t>> ExtractAMD64Regs(
    const MDRawContextAMD64& ctx);

// Collect ARM64 GPRs in tombstone display order.
// iregs layout: x0-x28 (0-28), fp/x29 (29), lr/x30 (30), sp (31), pc (32).
std::vector<std::pair<std::string, uint64_t>> ExtractARM64Regs(
    const MDRawContextARM64& ctx);

}  // namespace crashomon
