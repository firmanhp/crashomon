// daemon/minidump_patcher.h — patch missing BpEL build IDs in minidumps

#pragma once

#include <string>

#include "absl/status/status.h"

namespace crashomon {

// PatchMissingBuildIds reads the minidump at `path`, locates any module whose
// CodeView record is not a BpEL (MD_CVINFOELF_SIGNATURE) record, computes the
// Breakpad build ID from the ELF on disk (GNU .note.gnu.build-id first, then
// XOR-text-section fallback), and overwrites the CV record in-place.
//
// Returns OK even when no modules required patching.  Modules whose ELF is
// absent or lacks a .text section are silently skipped.  A write failure is
// reported as an InternalError.
absl::Status PatchMissingBuildIds(const std::string& path);

}  // namespace crashomon
