// daemon/test/test_minidump_patcher.cpp — tests for PatchMissingBuildIds

#include "daemon/minidump_patcher.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace crashomon {
namespace {

// ── Minidump layout constants ────────────────────────────────────────────────

constexpr uint32_t kMdmpSig = 0x504D444DU;
constexpr uint32_t kModuleListStream = 4U;  // MD_MODULE_LIST_STREAM
constexpr uint32_t kPdb70Sig = 0x53445352U;
constexpr uint32_t kBpElSig = 0x4270454CU;
constexpr size_t kModuleSize = 108;
constexpr size_t kModCvDataSizeOff = 76;
constexpr size_t kModCvRvaOff = 80;

// ── Buffer helpers ───────────────────────────────────────────────────────────

void W32(std::vector<char>& buf, size_t off, uint32_t val) {
  if (off + 4 <= buf.size()) {
    std::memcpy(buf.data() + off, &val, 4);
  }
}

void W64(std::vector<char>& buf, size_t off, uint64_t val) {
  if (off + 8 <= buf.size()) {
    std::memcpy(buf.data() + off, &val, 8);
  }
}

uint32_t R32(const std::vector<char>& buf, size_t off) {
  if (off + 4 > buf.size()) {
    return 0;
  }
  uint32_t val = 0;
  std::memcpy(&val, buf.data() + off, 4);
  return val;
}

// ── Minidump builder ─────────────────────────────────────────────────────────

// Build a minimal valid minidump with one module.  Mirrors Python's
// _build_test_minidump() from test_patchdmp.py for layout consistency.
std::vector<char> BuildTestMinidump(const std::string& module_path,
                                    const std::vector<char>& cv_bytes) {
  // UTF-16LE encode the path (ASCII only).
  std::vector<char> name_utf16;
  for (char c : module_path) {
    name_utf16.push_back(c);
    name_utf16.push_back('\0');
  }
  const uint32_t name_len = static_cast<uint32_t>(name_utf16.size());

  constexpr size_t kHeaderSize = 32;
  constexpr size_t kDirEntrySize = 12;

  const uint32_t stream_rva = static_cast<uint32_t>(kHeaderSize + kDirEntrySize);  // 44
  const uint32_t name_rva = stream_rva + 4U + static_cast<uint32_t>(kModuleSize);  // 156
  const uint32_t cv_rva = name_rva + 4U + name_len + 2U;
  const size_t total = cv_rva + cv_bytes.size();
  const uint32_t stream_data_size = 4U + static_cast<uint32_t>(kModuleSize);

  std::vector<char> buf(total, '\0');

  // MINIDUMP_HEADER
  W32(buf, 0, kMdmpSig);
  W32(buf, 4, 0x0000A793U);   // Version
  W32(buf, 8, 1U);             // NumberOfStreams
  W32(buf, 12, static_cast<uint32_t>(kHeaderSize));  // StreamDirectoryRva
  // CheckSum(4), TimeDateStamp(4), Flags(8) — already zero.

  // MINIDUMP_DIRECTORY[0]: StreamType, DataSize, Rva
  W32(buf, kHeaderSize + 0, kModuleListStream);
  W32(buf, kHeaderSize + 4, stream_data_size);
  W32(buf, kHeaderSize + 8, stream_rva);

  // Module list: count
  W32(buf, stream_rva, 1U);

  // MDRawModule at stream_rva + 4 = 48
  const size_t mod_off = stream_rva + 4;
  W64(buf, mod_off + 0, 0x7F000000ULL);  // BaseOfImage
  W32(buf, mod_off + 8, 0x10000U);       // SizeOfImage
  W32(buf, mod_off + kModCvDataSizeOff, static_cast<uint32_t>(cv_bytes.size()));
  W32(buf, mod_off + kModCvRvaOff, cv_rva);
  W32(buf, mod_off + 20, name_rva);  // ModuleNameRva

  // Module name: length(4) + UTF-16LE + null(2)
  W32(buf, name_rva, name_len);
  std::memcpy(buf.data() + name_rva + 4, name_utf16.data(), name_len);

  // CV record
  std::memcpy(buf.data() + cv_rva, cv_bytes.data(), cv_bytes.size());

  return buf;
}

std::string WriteTempFile(const std::vector<char>& data) {
  char tmpl[] = "/tmp/crashomon_patcher_test_XXXXXX";
  const int fd = mkstemp(tmpl);
  if (fd < 0) {
    return {};
  }
  const auto written = write(fd, data.data(), data.size());
  close(fd);
  if (static_cast<size_t>(written) != data.size()) {
    return {};
  }
  return tmpl;
}

std::vector<char> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return {};
  }
  const auto sz = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<char> result(sz);
  f.read(result.data(), static_cast<std::streamsize>(sz));
  return result;
}

// Build a PDB70 CV record for testing.
std::vector<char> Pdb70Record(const std::string& pdb_path = "") {
  std::vector<char> rec;
  rec.resize(4);
  std::memcpy(rec.data(), &kPdb70Sig, 4);
  rec.insert(rec.end(), 16, '\0');  // GUID
  rec.insert(rec.end(), 4, '\0');   // age
  rec.insert(rec.end(), pdb_path.begin(), pdb_path.end());
  rec.push_back('\0');
  return rec;
}

// ── Tests ────────────────────────────────────────────────────────────────────

// Compile a shared library without a build-ID note; verify PatchMissingBuildIds
// replaces the PDB70 CV record with a BpEL record.
TEST(MinidumpPatcher, PatchesPdb70WithXorFallback) {
  const std::string src = "/tmp/crashomon_patcher_src_test.c";
  const std::string lib = "/tmp/crashomon_patcher_lib_test.so";
  {
    std::ofstream f(src);
    f << "int patched_fn(void) { int x = 1; return x + 2; }\n";
  }
  ASSERT_EQ(
      system(("gcc -shared -fPIC -g -Wl,--build-id=none " + src + " -o " + lib).c_str()), 0)
      << "gcc compilation failed";

  const auto cv = Pdb70Record(lib);
  const auto dmp = BuildTestMinidump(lib, cv);
  const std::string path = WriteTempFile(dmp);
  ASSERT_FALSE(path.empty());

  // mod_off = stream_rva+4 = 44+4 = 48; cv_rva at mod_off+80 = 128
  constexpr size_t kCvRvaFieldOff = 128;
  constexpr size_t kCvSizeFieldOff = 124;

  const auto status = PatchMissingBuildIds(path);
  EXPECT_TRUE(status.ok()) << status.message();

  const auto result = ReadFile(path);
  const uint32_t cv_rva = R32(result, kCvRvaFieldOff);
  const uint32_t cv_size = R32(result, kCvSizeFieldOff);
  EXPECT_EQ(R32(result, cv_rva), kBpElSig);
  EXPECT_EQ(cv_size, 4U + 16U);  // 4-byte sig + 16-byte XOR hash

  std::remove(path.c_str());
  std::remove(src.c_str());
  std::remove(lib.c_str());
}

// A module with an existing BpEL record must not be re-patched.
TEST(MinidumpPatcher, SkipsBpElModules) {
  const std::string lib = "/tmp/crashomon_patcher_bpel_test.so";
  {
    std::ofstream f("/tmp/crashomon_patcher_bpel_src.c");
    f << "int skip(void) { return 0; }\n";
  }
  ASSERT_EQ(system(("gcc -shared -fPIC -Wl,--build-id=sha1 "
                    "/tmp/crashomon_patcher_bpel_src.c -o " +
                    lib)
                       .c_str()),
            0);

  // Build a BpEL CV record with a known 20-byte build ID.
  std::vector<char> bpel_cv(24, '\0');
  std::memcpy(bpel_cv.data(), &kBpElSig, 4);
  for (size_t j = 0; j < 20; ++j) {
    bpel_cv[4 + j] = static_cast<char>(j);
  }

  const auto dmp = BuildTestMinidump(lib, bpel_cv);
  const std::string path = WriteTempFile(dmp);
  ASSERT_FALSE(path.empty());

  const auto status = PatchMissingBuildIds(path);
  EXPECT_TRUE(status.ok());

  // File must be unchanged.
  const auto result = ReadFile(path);
  EXPECT_EQ(result, dmp);

  std::remove(path.c_str());
  std::remove("/tmp/crashomon_patcher_bpel_src.c");
  std::remove(lib.c_str());
}

// A module whose ELF is missing is silently skipped; the call still returns OK.
TEST(MinidumpPatcher, MissingElfSkipped) {
  const auto cv = Pdb70Record("/nonexistent/libghost.so");
  const auto dmp = BuildTestMinidump("/nonexistent/libghost.so", cv);
  const std::string path = WriteTempFile(dmp);
  ASSERT_FALSE(path.empty());

  const auto status = PatchMissingBuildIds(path);
  EXPECT_TRUE(status.ok());

  // CV record must still be PDB70 (not patched).
  const auto result = ReadFile(path);
  constexpr size_t kCvRvaFieldOff = 128;
  const uint32_t cv_rva = R32(result, kCvRvaFieldOff);
  EXPECT_EQ(R32(result, cv_rva), kPdb70Sig);

  std::remove(path.c_str());
}

}  // namespace
}  // namespace crashomon
