// daemon/minidump_patcher.cpp — patch missing BpEL build IDs in minidumps

#include "daemon/minidump_patcher.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "spdlog/spdlog.h"

namespace crashomon {
namespace {

// ── Minidump layout constants ────────────────────────────────────────────────

constexpr uint32_t kMdmpSignature = 0x504D444DU;
constexpr uint32_t kModuleListStream = 3U;
constexpr uint32_t kBpElSignature = 0x4270454CU;  // MD_CVINFOELF_SIGNATURE

// MDRawModule field offsets and size.
constexpr size_t kModuleSize = 108;
constexpr size_t kModNameRvaOff = 20;
constexpr size_t kModCvDataSizeOff = 76;
constexpr size_t kModCvRvaOff = 80;

// MINIDUMP_DIRECTORY entry size: StreamType(4) + DataSize(4) + Rva(4).
constexpr size_t kDirEntrySize = 12;

// ELF fallback constants — must match Breakpad's HashElfTextSection().
constexpr size_t kXorHashSize = 16;   // kMDGUIDSize
constexpr size_t kXorMaxBytes = 4096;
constexpr uint32_t kNtGnuBuildId = 3;  // NT_GNU_BUILD_ID note type

// ── Buffer helpers ───────────────────────────────────────────────────────────

uint32_t ReadU32(const std::vector<char>& buf, size_t off) noexcept {
  if (off + 4 > buf.size()) {
    return 0;
  }
  uint32_t val = 0;
  std::memcpy(&val, buf.data() + off, 4);
  return val;
}

void WriteU32(std::vector<char>& buf, size_t off, uint32_t val) noexcept {
  if (off + 4 > buf.size()) {
    return;
  }
  std::memcpy(buf.data() + off, &val, 4);
}

// Decode a UTF-16LE module name from a minidump buffer (ASCII fast path).
std::string ReadModuleName(const std::vector<char>& buf, uint32_t rva) {
  if (rva == 0 || rva + 4 > buf.size()) {
    return {};
  }
  const uint32_t len = ReadU32(buf, rva);
  if (len == 0 || len % 2 != 0 || rva + 4 + len > buf.size()) {
    return {};
  }
  std::string path;
  path.reserve(len / 2);
  for (uint32_t i = 0; i < len; i += 2) {
    const auto lo = static_cast<uint8_t>(buf[rva + 4 + i]);
    const auto hi = static_cast<uint8_t>(buf[rva + 4 + i + 1]);
    const uint32_t ch = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 8U);
    if (ch < 128U && ch != 0U) {
      path += static_cast<char>(ch);
    }
  }
  return path;
}

// ── ELF build-ID reader ──────────────────────────────────────────────────────

// Extract NT_GNU_BUILD_ID desc bytes from an ELF note region.
std::vector<uint8_t> ParseBuildIdNote(const uint8_t* base, size_t size) {
  size_t pos = 0;
  while (pos + 12 <= size) {
    uint32_t namesz = 0;
    uint32_t descsz = 0;
    uint32_t ntype = 0;
    std::memcpy(&namesz, base + pos, 4);
    std::memcpy(&descsz, base + pos + 4, 4);
    std::memcpy(&ntype, base + pos + 8, 4);
    pos += 12;
    const size_t namesz_padded = (namesz + 3U) & ~size_t{3};
    const size_t descsz_padded = (descsz + 3U) & ~size_t{3};
    if (ntype == kNtGnuBuildId && descsz > 0) {
      const size_t desc_off = pos + namesz_padded;
      if (desc_off + descsz <= size) {
        return {base + desc_off, base + desc_off + descsz};
      }
    }
    pos += namesz_padded + descsz_padded;
  }
  return {};
}

struct SectionFields {
  uint32_t sh_name;
  uint32_t sh_type;
  uint64_t sh_offset;
  uint64_t sh_size;
};

SectionFields ReadShdr(const char* base, uint64_t e_shoff, uint16_t e_shentsize, uint16_t index,
                       bool is64) {
  const char* shdr = base + e_shoff + static_cast<size_t>(index) * e_shentsize;
  SectionFields f{};
  if (is64) {
    const auto* s = reinterpret_cast<const Elf64_Shdr*>(shdr);
    f.sh_name = s->sh_name;
    f.sh_type = s->sh_type;
    f.sh_offset = s->sh_offset;
    f.sh_size = s->sh_size;
  } else {
    const auto* s = reinterpret_cast<const Elf32_Shdr*>(shdr);
    f.sh_name = s->sh_name;
    f.sh_type = s->sh_type;
    f.sh_offset = s->sh_offset;
    f.sh_size = s->sh_size;
  }
  return f;
}

// Compute the Breakpad build ID for an ELF binary.
// Priority: GNU .note.gnu.build-id → XOR-text-section fallback.
// Returns empty on failure (not ELF, no .text, etc.).
std::vector<uint8_t> ComputeElfBuildId(const std::string& elf_path) {
  const int fd = open(elf_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }

  struct stat st{};
  if (fstat(fd, &st) != 0) {
    close(fd);
    return {};
  }
  const size_t file_size = static_cast<size_t>(st.st_size);
  if (file_size < 64U) {  // minimum Elf64_Ehdr
    close(fd);
    return {};
  }

  void* map = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    return {};
  }

  const auto* base = static_cast<const char*>(map);
  const auto* ubase = static_cast<const uint8_t*>(map);

  if (base[0] != '\x7f' || base[1] != 'E' || base[2] != 'L' || base[3] != 'F') {
    munmap(map, file_size);
    return {};
  }

  const bool is64 = (ubase[4] == ELFCLASS64);

  uint64_t e_shoff = 0;
  uint16_t e_shentsize = 0;
  uint16_t e_shnum = 0;
  uint16_t e_shstrndx = 0;
  if (is64) {
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(base);
    e_shoff = ehdr->e_shoff;
    e_shentsize = ehdr->e_shentsize;
    e_shnum = ehdr->e_shnum;
    e_shstrndx = ehdr->e_shstrndx;
  } else {
    const auto* ehdr = reinterpret_cast<const Elf32_Ehdr*>(base);
    e_shoff = ehdr->e_shoff;
    e_shentsize = ehdr->e_shentsize;
    e_shnum = ehdr->e_shnum;
    e_shstrndx = ehdr->e_shstrndx;
  }

  if (e_shoff == 0 || e_shstrndx == SHN_UNDEF ||
      e_shoff + static_cast<uint64_t>(e_shnum) * e_shentsize > file_size) {
    munmap(map, file_size);
    return {};
  }

  const SectionFields strtab_shdr = ReadShdr(base, e_shoff, e_shentsize, e_shstrndx, is64);
  if (strtab_shdr.sh_offset >= file_size) {
    munmap(map, file_size);
    return {};
  }
  const char* shstrtab = base + strtab_shdr.sh_offset;

  uint64_t text_off = 0;
  uint64_t text_size = 0;

  for (uint16_t i = 0; i < e_shnum; ++i) {
    const SectionFields shdr = ReadShdr(base, e_shoff, e_shentsize, i, is64);
    if (strtab_shdr.sh_offset + shdr.sh_name >= file_size) {
      continue;
    }
    const char* sec_name = shstrtab + shdr.sh_name;

    if (shdr.sh_type == SHT_NOTE && std::strcmp(sec_name, ".note.gnu.build-id") == 0) {
      if (shdr.sh_offset + shdr.sh_size <= file_size) {
        auto bid = ParseBuildIdNote(ubase + shdr.sh_offset, shdr.sh_size);
        if (!bid.empty()) {
          munmap(map, file_size);
          return bid;
        }
      }
    }
    if (shdr.sh_type == SHT_PROGBITS && text_off == 0 &&
        std::strcmp(sec_name, ".text") == 0) {
      text_off = shdr.sh_offset;
      text_size = shdr.sh_size;
    }
  }

  if (text_off == 0 || text_size == 0 || text_off + text_size > file_size) {
    munmap(map, file_size);
    return {};
  }

  // XOR fallback: replicates Breakpad's HashElfTextSection().
  // mmap is zero-padded to the next page boundary, so the last partial chunk
  // yields zeros beyond text_size — matching Breakpad's mmap-backed behavior.
  std::vector<uint8_t> hash(kXorHashSize, 0);
  const auto* ptr = ubase + text_off;
  const size_t limit = std::min(static_cast<size_t>(text_size), kXorMaxBytes);
  for (size_t j = 0; j < limit; j += kXorHashSize) {
    for (size_t k = 0; k < kXorHashSize; ++k) {
      hash[k] ^= ptr[j + k];
    }
  }

  munmap(map, file_size);
  return hash;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

absl::Status PatchMissingBuildIds(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return absl::InternalError(absl::StrCat("cannot open minidump: ", path));
  }
  const auto file_size = static_cast<size_t>(file.tellg());
  file.seekg(0);
  std::vector<char> buf(file_size);
  if (!file.read(buf.data(), static_cast<std::streamsize>(file_size))) {
    return absl::InternalError(absl::StrCat("read failed: ", path));
  }
  file.close();

  if (buf.size() < 32 || ReadU32(buf, 0) != kMdmpSignature) {
    return absl::InvalidArgumentError(absl::StrCat("not a minidump: ", path));
  }

  const uint32_t stream_count = ReadU32(buf, 8);
  const uint32_t dir_rva = ReadU32(buf, 12);

  uint32_t module_stream_rva = 0;
  for (uint32_t i = 0; i < stream_count; ++i) {
    const size_t entry_off = dir_rva + static_cast<size_t>(i) * kDirEntrySize;
    if (entry_off + kDirEntrySize > buf.size()) {
      break;
    }
    if (ReadU32(buf, entry_off) == kModuleListStream) {
      module_stream_rva = ReadU32(buf, entry_off + 8);
      break;
    }
  }
  if (module_stream_rva == 0 || module_stream_rva + 4 > buf.size()) {
    return absl::OkStatus();
  }

  const uint32_t module_count = ReadU32(buf, module_stream_rva);
  if (module_count > 4096U) {
    return absl::OkStatus();
  }

  int patched = 0;

  for (uint32_t i = 0; i < module_count; ++i) {
    const size_t mod_off = module_stream_rva + 4 + static_cast<size_t>(i) * kModuleSize;
    if (mod_off + kModuleSize > buf.size()) {
      break;
    }

    const uint32_t name_rva = ReadU32(buf, mod_off + kModNameRvaOff);
    const uint32_t cv_size = ReadU32(buf, mod_off + kModCvDataSizeOff);
    const uint32_t cv_rva = ReadU32(buf, mod_off + kModCvRvaOff);

    if (cv_rva == 0 || cv_size < 4U + static_cast<uint32_t>(kXorHashSize)) {
      continue;
    }
    if (cv_rva + cv_size > buf.size()) {
      continue;
    }
    if (ReadU32(buf, cv_rva) == kBpElSignature) {
      continue;
    }

    const std::string module_path = ReadModuleName(buf, name_rva);
    if (module_path.empty()) {
      continue;
    }

    const std::vector<uint8_t> build_id = ComputeElfBuildId(module_path);
    if (build_id.empty()) {
      spdlog::debug("patchdmp: skip {} (no .text or unreadable ELF)", module_path);
      continue;
    }

    const uint32_t new_cv_size = 4U + static_cast<uint32_t>(build_id.size());
    if (cv_size < new_cv_size) {
      continue;
    }

    WriteU32(buf, cv_rva, kBpElSignature);
    std::memcpy(buf.data() + cv_rva + 4, build_id.data(), build_id.size());
    WriteU32(buf, mod_off + kModCvDataSizeOff, new_cv_size);

    spdlog::info("patchdmp: patched build ID for {}", module_path);
    ++patched;
  }

  if (patched == 0) {
    return absl::OkStatus();
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return absl::InternalError(absl::StrCat("write failed: ", path));
  }
  out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  if (!out) {
    return absl::InternalError(absl::StrCat("flush failed: ", path));
  }
  return absl::OkStatus();
}

}  // namespace crashomon
