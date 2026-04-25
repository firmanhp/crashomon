// test/test_minidump_reader.cpp — unit tests for MinidumpReader
//
// These tests require real .dmp fixture files produced by gen_fixtures.sh.
// If the fixtures directory is absent or a specific .dmp is missing, the
// relevant test is skipped with GTEST_SKIP() rather than failing.
//
// Fixture location (in order of precedence):
//   1. CRASHOMON_FIXTURES_DIR environment variable
//   2. A "fixtures" subdirectory next to the test binary

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "daemon/tombstone/minidump_reader.h"

namespace crashomon {
namespace {

// ── Fixture path helpers ────────────────────────────────────────────────────

// Returns the directory that holds the .dmp fixtures (may not exist).
std::filesystem::path FixturesDir() {
  const char* env = std::getenv("CRASHOMON_FIXTURES_DIR");
  if (env != nullptr && *env != '\0') {
    return env;
  }

  // Fall back to a "fixtures" subdir next to the running test binary.
  // argv[0] is not easily portable from a test body, but
  // GTEST_BINARY_DIR is set by CMake via gtest_discover_tests().
  const char* bin_dir = std::getenv("GTEST_BINARY_DIR");
  if (bin_dir != nullptr && *bin_dir != '\0') {
    return std::filesystem::path(bin_dir) / "fixtures";
  }

  // Last resort: relative path from the cwd (works when run from repo root).
  return std::filesystem::path("test") / "fixtures";
}

// Returns the path to a fixture file; an empty path signals "skip this test".
std::filesystem::path FixturePath(const std::string& name) {
  auto fixture_path = FixturesDir() / (name + ".dmp");  // non-const to enable move on return
  if (!std::filesystem::exists(fixture_path)) {
    return {};
  }
  return fixture_path;
}

// ── Error-path tests (no fixture files required) ────────────────────────────

TEST(MinidumpReaderTest, NonexistentFile) {
  auto result = ReadMinidump("/nonexistent/path/does_not_exist.dmp");
  EXPECT_FALSE(result.ok());
}

TEST(MinidumpReaderTest, EmptyFile) {
  // Write a zero-byte temp file and confirm the parser rejects it.
  // std::string provides a mutable buffer for mkstemp.
  std::string tmpl = "/tmp/crashomon_empty_XXXXXX";
  // fd is the universal POSIX idiom for file
  // descriptor; renaming would reduce clarity.
  // NOLINTNEXTLINE(readability-identifier-length)
  const int fd =
      mkstemp(tmpl.data());  // NOLINT(misc-include-cleaner) — mkstemp comes via <cstdlib> which is
                             // included; false positive from include-cleaner.
  ASSERT_GE(fd, 0);
  ::close(fd);

  auto result = ReadMinidump(tmpl);
  EXPECT_FALSE(result.ok());

  ::unlink(tmpl.c_str());
}

TEST(MinidumpReaderTest, GarbageFile) {
  // Write random bytes and confirm the parser rejects it.
  // std::string provides a mutable buffer for mkstemp.
  std::string tmpl = "/tmp/crashomon_garbage_XXXXXX";
  // fd is the universal POSIX idiom for file
  // descriptor; renaming would reduce clarity.
  // NOLINTNEXTLINE(readability-identifier-length)
  const int fd =
      mkstemp(tmpl.data());  // NOLINT(misc-include-cleaner) — mkstemp comes via <cstdlib> which is
                             // included; false positive from include-cleaner.
  ASSERT_GE(fd, 0);
  // std::string with length constructor preserves embedded nulls.
  constexpr size_t junk_size = 18;
  const std::string junk{"not a minidump\x00\x01\x02\x03", junk_size};
  // NOLINTNEXTLINE(misc-include-cleaner) — write() comes via <unistd.h> which is included.
  [[maybe_unused]] const ssize_t unused = ::write(fd, junk.data(), junk.size());
  ::close(fd);

  auto result = ReadMinidump(tmpl);
  EXPECT_FALSE(result.ok());

  ::unlink(tmpl.c_str());
}

// ── Fixture-based tests ─────────────────────────────────────────────────────

// Helper macro: skip the test if the named fixture is absent.
// Must be a macro so the variable declaration and GTEST_SKIP() appear at call-site scope.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
#define REQUIRE_FIXTURE(name, path_var)                                       \
  auto path_var = FixturePath(name); /* NOLINT(bugprone-macro-parentheses) */ \
  if ((path_var).empty()) {                                                   \
    GTEST_SKIP() << "Fixture '" name ".dmp' not found in " << FixturesDir()   \
                 << " — run test/gen_fixtures.sh";                            \
  }

// ── segfault.dmp ─────────────────────────────────────────────────────────────

TEST(MinidumpReaderTest, SegfaultFixture_ParsesOk) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(MinidumpReaderTest, SegfaultFixture_HasSigsegv) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  // signal_info format: "SIGSEGV / SEGV_MAPERR" or similar.
  EXPECT_NE(result->signal_info.find("SIGSEGV"), std::string::npos)
      << "signal_info: " << result->signal_info;
}

TEST(MinidumpReaderTest, SegfaultFixture_HasAtLeastOneThread) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->threads.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_CrashingThreadFirst) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

TEST(MinidumpReaderTest, SegfaultFixture_CrashingThreadHasFrames) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_FALSE(result->threads[0].frames.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_CrashingThreadHasRegisters) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  // AMD64 registers should be populated for the crashing thread.
  EXPECT_FALSE(result->threads[0].registers.empty());
  // RIP must be present.
  bool found_rip = false;
  for (const auto& [name, val] : result->threads[0].registers) {
    if (name == "rip") {
      found_rip = true;
      break;
    }
  }
  EXPECT_TRUE(found_rip) << "Expected 'rip' in crashing thread registers";
}

TEST(MinidumpReaderTest, SegfaultFixture_HasModules) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->modules.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_ProcessNameNonEmpty) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->process_name.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_TimestampNonEmpty) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_FALSE(result->timestamp.empty());
}

TEST(MinidumpReaderTest, SegfaultFixture_MinidumpPathPreserved) {
  REQUIRE_FIXTURE("segfault", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->minidump_path, path.string());
}

// ── abort.dmp ────────────────────────────────────────────────────────────────

TEST(MinidumpReaderTest, AbortFixture_ParsesOk) {
  REQUIRE_FIXTURE("abort", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(MinidumpReaderTest, AbortFixture_HasSigabrt) {
  REQUIRE_FIXTURE("abort", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_NE(result->signal_info.find("SIGABRT"), std::string::npos)
      << "signal_info: " << result->signal_info;
}

TEST(MinidumpReaderTest, AbortFixture_HasCrashingThread) {
  REQUIRE_FIXTURE("abort", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

// ── multithread.dmp ──────────────────────────────────────────────────────────

TEST(MinidumpReaderTest, MultithreadFixture_ParsesOk) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
}

TEST(MinidumpReaderTest, MultithreadFixture_HasCrashingThread) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  // Only the crashing thread is collected; non-crashing threads are not included.
  EXPECT_EQ(result->threads.size(), 1U);
  EXPECT_TRUE(result->threads[0].is_crashing);
}

TEST(MinidumpReaderTest, MultithreadFixture_CrashingThreadIsFirst) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

TEST(MinidumpReaderTest, MultithreadFixture_OnlyOneThreadIsCrashing) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  int crashing_count = 0;
  for (const auto& thr : result->threads) {
    if (thr.is_crashing) {
      ++crashing_count;
    }
  }
  EXPECT_EQ(crashing_count, 1);
}

TEST(MinidumpReaderTest, MultithreadFixture_NonCrashingThreadsHaveNoRegisters) {
  REQUIRE_FIXTURE("multithread", path);
  auto result = ReadMinidump(path.string());
  ASSERT_TRUE(result.ok()) << result.status();
  for (size_t i = 1; i < result->threads.size(); ++i) {
    EXPECT_TRUE(result->threads[i].registers.empty())
        << "Thread " << i << " is not the crashing thread but has registers";
  }
}

// ── Helpers for annotation tests ─────────────────────────────────────────────

namespace annot_helpers {

void AppendU32(std::string& buf, uint32_t val) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  buf.append(reinterpret_cast<const char*>(&val), sizeof(val));
}

void AppendUtf8String(std::string& buf, const std::string& str) {
  AppendU32(buf, static_cast<uint32_t>(str.size()));
  buf.append(str);
  buf.push_back('\0');
}

// Builds a minimal valid MDMP binary with a CrashpadInfo stream (type 0x43500007)
// containing abort_message (and optionally terminate_type).
std::string BuildAnnotatedMinidump(const std::string& abort_msg, const std::string& term_type) {
  static constexpr uint32_t kMdmpSignature = 0x504d444dU;
  static constexpr uint32_t kMdmpVersion = 0xa793U;
  static constexpr uint32_t kCrashpadInfoType = 0x43500007U;
  static constexpr uint32_t kHeaderSize = 32U;
  static constexpr uint32_t kDirEntrySize = 12U;
  static constexpr uint32_t kCrashpadHdrSize = 52U;
  static constexpr uint32_t kFlagsBytes = 8U;
  static constexpr uint32_t kUuidBytes = 16U;
  static constexpr uint32_t kDirRva = kHeaderSize;
  static constexpr uint32_t kStreamRva = kDirRva + kDirEntrySize;
  static constexpr uint32_t kStrLenFieldSize = 4U;
  static constexpr uint32_t kDictCountSize = 4U;
  static constexpr uint32_t kDictEntrySize = 8U;

  const uint32_t num_entries = term_type.empty() ? 1U : 2U;
  const uint32_t dict_rva = kStreamRva + kCrashpadHdrSize;
  const uint32_t dict_bytes = kDictCountSize + num_entries * kDictEntrySize;
  const uint32_t strings_rva = dict_rva + dict_bytes;

  auto str_total = [](const std::string& str) -> uint32_t {
    return kStrLenFieldSize + static_cast<uint32_t>(str.size()) + 1U;
  };

  constexpr std::string_view key_abort = "abort_message";
  constexpr std::string_view key_type = "terminate_type";

  const uint32_t key0_rva = strings_rva;
  const uint32_t val0_rva = key0_rva + str_total(std::string(key_abort));
  const uint32_t key1_rva = val0_rva + str_total(abort_msg);
  const uint32_t val1_rva = key1_rva + str_total(std::string(key_type));

  uint32_t total_strings = str_total(std::string(key_abort)) + str_total(abort_msg);
  if (!term_type.empty()) {
    total_strings += str_total(std::string(key_type)) + str_total(term_type);
  }
  const uint32_t stream_size = kCrashpadHdrSize + dict_bytes + total_strings;

  std::string buf;
  buf.reserve(kHeaderSize + kDirEntrySize + stream_size);

  // MDMP header.
  AppendU32(buf, kMdmpSignature);
  AppendU32(buf, kMdmpVersion);
  AppendU32(buf, 1U);             // stream_count
  AppendU32(buf, kDirRva);        // stream_directory_rva
  AppendU32(buf, 0U);             // checksum
  AppendU32(buf, 0U);             // time_date_stamp
  buf.append(kFlagsBytes, '\0');  // flags (uint64_t)

  // Directory entry.
  AppendU32(buf, kCrashpadInfoType);
  AppendU32(buf, stream_size);
  AppendU32(buf, kStreamRva);

  // MinidumpCrashpadInfo header.
  AppendU32(buf, 1U);            // version
  buf.append(kUuidBytes, '\0');  // report_id
  buf.append(kUuidBytes, '\0');  // client_id
  AppendU32(buf, dict_bytes);    // simple_annotations.DataSize
  AppendU32(buf, dict_rva);      // simple_annotations.RVA
  AppendU32(buf, 0U);            // module_list.DataSize
  AppendU32(buf, 0U);            // module_list.RVA

  // SimpleStringDictionary.
  AppendU32(buf, num_entries);
  AppendU32(buf, key0_rva);
  AppendU32(buf, val0_rva);
  if (!term_type.empty()) {
    AppendU32(buf, key1_rva);
    AppendU32(buf, val1_rva);
  }

  // String data.
  AppendUtf8String(buf, std::string(key_abort));
  AppendUtf8String(buf, abort_msg);
  if (!term_type.empty()) {
    AppendUtf8String(buf, std::string(key_type));
    AppendUtf8String(buf, term_type);
  }

  return buf;
}

std::string WriteTmpFile(const std::string& buf) {
  std::string tmpl = "/tmp/crashomon_annot_XXXXXX";
  const int tmp_fd = mkstemp(tmpl.data());
  if (tmp_fd < 0) {
    return {};
  }
  // NOLINTNEXTLINE(misc-include-cleaner) — write() comes via <unistd.h> which is included.
  [[maybe_unused]] const ssize_t unused = ::write(tmp_fd, buf.data(), buf.size());
  ::close(tmp_fd);
  return tmpl;
}

}  // namespace annot_helpers

// ── Annotation reader tests ──────────────────────────────────────────────────

TEST(MinidumpReaderAnnotationTest, ExtractsAbortMessageFromCrashpadStream) {
  const std::string dmp = annot_helpers::WriteTmpFile(
      annot_helpers::BuildAnnotatedMinidump("assertion failed: 'x > 0' (main.cpp:7, run())", ""));
  ASSERT_FALSE(dmp.empty());
  auto result = ReadMinidump(dmp);
  ::unlink(dmp.c_str());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->abort_message, "assertion failed: 'x > 0' (main.cpp:7, run())");
  EXPECT_TRUE(result->terminate_type.empty());
}

TEST(MinidumpReaderAnnotationTest, ExtractsBothAbortMessageAndTerminateType) {
  const std::string dmp = annot_helpers::WriteTmpFile(
      annot_helpers::BuildAnnotatedMinidump("unhandled C++ exception", "std::logic_error"));
  ASSERT_FALSE(dmp.empty());
  auto result = ReadMinidump(dmp);
  ::unlink(dmp.c_str());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->abort_message, "unhandled C++ exception");
  EXPECT_EQ(result->terminate_type, "std::logic_error");
}

TEST(MinidumpReaderAnnotationTest, AbortMessageOnlyOneEntry) {
  const std::string dmp =
      annot_helpers::WriteTmpFile(annot_helpers::BuildAnnotatedMinidump("msg", ""));
  ASSERT_FALSE(dmp.empty());
  auto result = ReadMinidump(dmp);
  ::unlink(dmp.c_str());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_EQ(result->abort_message, "msg");
  EXPECT_TRUE(result->terminate_type.empty());
}

TEST(MinidumpReaderAnnotationTest, AnnotationFieldsEmptyWhenCrashpadStreamAbsent) {
  // Build a minimal valid MDMP header with 0 streams.
  // ExtractCrashpadAnnotations should find no CrashpadInfo stream and leave both
  // fields at their default empty values.
  constexpr uint32_t mdmp_signature = 0x504d444dU;
  constexpr uint32_t mdmp_version = 0xa793U;
  constexpr uint32_t mdmp_header_size = 32U;
  constexpr size_t flags_bytes = 8U;

  std::string buf;
  buf.reserve(mdmp_header_size);
  annot_helpers::AppendU32(buf, mdmp_signature);   // MDMP signature
  annot_helpers::AppendU32(buf, mdmp_version);      // version
  annot_helpers::AppendU32(buf, 0U);                // stream_count = 0
  annot_helpers::AppendU32(buf, mdmp_header_size);  // stream_directory_rva (unused)
  annot_helpers::AppendU32(buf, 0U);                // checksum
  annot_helpers::AppendU32(buf, 0U);                // time_date_stamp
  buf.append(flags_bytes, '\0');                    // flags (uint64_t)

  const std::string dmp = annot_helpers::WriteTmpFile(buf);
  ASSERT_FALSE(dmp.empty());
  auto result = ReadMinidump(dmp);
  ::unlink(dmp.c_str());
  ASSERT_TRUE(result.ok()) << result.status();
  EXPECT_TRUE(result->abort_message.empty());
  EXPECT_TRUE(result->terminate_type.empty());
}

}  // namespace
}  // namespace crashomon
