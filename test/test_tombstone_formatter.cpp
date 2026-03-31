// test/test_tombstone_formatter.cpp — unit tests for FormatTombstone

#include <cstddef>
#include <cstdint>
#include <string>

#include "daemon/minidump_reader.h"
#include "daemon/tombstone_formatter.h"
#include "gtest/gtest.h"

namespace crashomon {
namespace {

// ── Test constants ────────────────────────────────────────────────────────────
constexpr uint32_t kTestPid = 1234;
constexpr uint32_t kTestTid = 1234;
constexpr uint32_t kTestSigSegv = 11;
constexpr uint32_t kTestSigAbrt = 6;
constexpr uint32_t kTestSigCode1 = 1;
constexpr uint64_t kTestFaultAddr = 0xdeadbeef00000000ULL;
constexpr uint32_t kTestTid2 = 1235;
constexpr uint32_t kTestTid3 = 9999;
constexpr uint64_t kTestUnmappedPc = 0xbadf00d0ULL;
// Frame PCs and offsets.
constexpr uint64_t kFrame0Pc = 0x5555000011a0ULL;
constexpr uint64_t kFrame0Offset = 0x11a0ULL;
constexpr uint64_t kFrame1Pc = 0x5555000010f0ULL;
constexpr uint64_t kFrame1Offset = 0x10f0ULL;
constexpr uint64_t kFrame2Pc = 0x7f0000023b09ULL;
constexpr uint64_t kFrame2Offset = 0x23b09ULL;
constexpr uint64_t kThread2Frame0Pc = 0x7f0000010000ULL;
constexpr uint64_t kThread2Frame0Offset = 0x10000ULL;

// Build a minimal MinidumpInfo for testing.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
MinidumpInfo MakeInfo() {
  MinidumpInfo
      info;  // NOLINT(misc-include-cleaner) — MinidumpInfo comes via daemon/minidump_reader.h which
             // is included; false positive from include-cleaner.
  info.pid = kTestPid;
  info.crashing_tid = kTestTid;
  info.process_name = "my_service";
  info.signal_info = "SIGSEGV / SEGV_MAPERR";
  info.signal_number = kTestSigSegv;
  info.signal_code = kTestSigCode1;
  info.fault_addr = kTestFaultAddr;
  info.timestamp = "2026-03-27T10:15:30Z";
  info.minidump_path = "/var/crashomon/test.dmp";

  ThreadInfo
      crashing;  // NOLINT(misc-include-cleaner) — ThreadInfo comes via daemon/minidump_reader.h
                 // which is included; false positive from include-cleaner.
  crashing.tid = kTestTid;
  crashing.is_crashing = true;
  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) — register values
  // are sequential test data; naming each 0x0-0x10 would be less readable.
  crashing.registers = {
      {"rax", 0x0}, {"rbx", 0x1}, {"rcx", 0x2}, {"rdx", 0x3}, {"rsi", 0x4},  {"rdi", 0x5},
      {"rbp", 0x6}, {"rsp", 0x7}, {"r8", 0x8},  {"r9", 0x9},  {"r10", 0xa},  {"r11", 0xb},
      {"r12", 0xc}, {"r13", 0xd}, {"r14", 0xe}, {"r15", 0xf}, {"rip", 0x10},
  };
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  crashing.frames = {
      {kFrame0Pc, kFrame0Offset, "/usr/bin/my_service"},
      {kFrame1Pc, kFrame1Offset, "/usr/bin/my_service"},
      {kFrame2Pc, kFrame2Offset, "/usr/lib/libc.so.6"},
  };
  info.threads.push_back(crashing);

  return info;
}

TEST(TombstoneFormatterTest, ContainsSeparators) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("*** *** ***"), std::string::npos);
  // Should appear twice: at start and end.
  const size_t first = tomb.find("*** *** ***");
  const size_t last = tomb.rfind("*** *** ***");
  EXPECT_NE(first, last);
}

TEST(TombstoneFormatterTest, ContainsPidTid) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("pid: 1234"), std::string::npos);
  EXPECT_NE(tomb.find("tid: 1234"), std::string::npos);
  EXPECT_NE(tomb.find("my_service"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsSignalLine) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("signal 11"), std::string::npos);
  EXPECT_NE(tomb.find("SIGSEGV"), std::string::npos);
  EXPECT_NE(tomb.find("SEGV_MAPERR"), std::string::npos);
  EXPECT_NE(tomb.find("code 1"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsFaultAddr) {
  auto tomb = FormatTombstone(MakeInfo());
  // fault_addr = 0xdeadbeef00000000 → should appear in hex
  EXPECT_NE(tomb.find("deadbeef00000000"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsTimestamp) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("2026-03-27T10:15:30Z"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsRegisters) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("rax"), std::string::npos);
  EXPECT_NE(tomb.find("rip"), std::string::npos);
  // rip = 0x10 → "0000000000000010"
  EXPECT_NE(tomb.find("0000000000000010"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsBacktrace) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("backtrace:"), std::string::npos);
  EXPECT_NE(tomb.find("#0"), std::string::npos);
  EXPECT_NE(tomb.find("/usr/bin/my_service"), std::string::npos);
  EXPECT_NE(tomb.find("/usr/lib/libc.so.6"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsMinidumpPath) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("/var/crashomon/test.dmp"), std::string::npos);
}

TEST(TombstoneFormatterTest, NoCodeNameWhenSignalInfoHasNoSlash) {
  MinidumpInfo info = MakeInfo();
  info.signal_info = "SIGABRT";
  info.signal_number = kTestSigAbrt;
  info.signal_code = 0;
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("signal 6"), std::string::npos);
  EXPECT_NE(tomb.find("SIGABRT"), std::string::npos);
  // No " code " when there is no slash in signal_info.
  EXPECT_EQ(tomb.find("code 0"), std::string::npos);
}

TEST(TombstoneFormatterTest, MultipleThreads) {
  MinidumpInfo info = MakeInfo();

  ThreadInfo idle;
  idle.tid = kTestTid2;
  idle.is_crashing = false;
  idle.frames = {{kThread2Frame0Pc, kThread2Frame0Offset, "/usr/lib/libc.so.6"}};
  info.threads.push_back(idle);

  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("--- --- --- thread 1235"), std::string::npos);
}

TEST(TombstoneFormatterTest, ThreadWithName) {
  MinidumpInfo info = MakeInfo();

  ThreadInfo named;
  named.tid = kTestTid3;
  named.name = "worker";
  named.is_crashing = false;
  info.threads.push_back(named);

  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("worker"), std::string::npos);
}

TEST(TombstoneFormatterTest, UnmappedFrame) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].frames = {{kTestUnmappedPc, 0, ""}};
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("???"), std::string::npos);
}

TEST(TombstoneFormatterTest, EmptyRegisters) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].registers.clear();
  auto tomb = FormatTombstone(info);
  // Tombstone still valid without register block.
  EXPECT_NE(tomb.find("backtrace:"), std::string::npos);
}

TEST(TombstoneFormatterTest, FaultAddrZero) {
  MinidumpInfo info = MakeInfo();
  info.fault_addr = 0;
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("0000000000000000"), std::string::npos);
}

}  // namespace
}  // namespace crashomon
