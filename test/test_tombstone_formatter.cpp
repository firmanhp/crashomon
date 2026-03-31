// test/test_tombstone_formatter.cpp — unit tests for FormatTombstone

#include "daemon/tombstone_formatter.h"

#include <string>

#include "gtest/gtest.h"

namespace crashomon {
namespace {

// Build a minimal MinidumpInfo for testing.
MinidumpInfo MakeInfo() {
  MinidumpInfo info;
  info.pid = 1234;
  info.crashing_tid = 1234;
  info.process_name = "my_service";
  info.signal_info = "SIGSEGV / SEGV_MAPERR";
  info.signal_number = 11;
  info.signal_code = 1;
  info.fault_addr = 0xdeadbeef00000000ULL;
  info.timestamp = "2026-03-27T10:15:30Z";
  info.minidump_path = "/var/crashomon/test.dmp";

  ThreadInfo crashing;
  crashing.tid = 1234;
  crashing.is_crashing = true;
  crashing.registers = {
    {"rax", 0x0}, {"rbx", 0x1}, {"rcx", 0x2},
    {"rdx", 0x3}, {"rsi", 0x4}, {"rdi", 0x5},
    {"rbp", 0x6}, {"rsp", 0x7}, {"r8",  0x8},
    {"r9",  0x9}, {"r10", 0xa}, {"r11", 0xb},
    {"r12", 0xc}, {"r13", 0xd}, {"r14", 0xe},
    {"r15", 0xf}, {"rip", 0x10},
  };
  crashing.frames = {
    {0x5555000011a0ULL, 0x11a0ULL, "/usr/bin/my_service"},
    {0x5555000010f0ULL, 0x10f0ULL, "/usr/bin/my_service"},
    {0x7f0000023b09ULL, 0x23b09ULL, "/usr/lib/libc.so.6"},
  };
  info.threads.push_back(crashing);

  return info;
}

TEST(TombstoneFormatterTest, ContainsSeparators) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("*** *** ***"), std::string::npos);
  // Should appear twice: at start and end.
  size_t first = tomb.find("*** *** ***");
  size_t last = tomb.rfind("*** *** ***");
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
  info.signal_number = 6;
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
  idle.tid = 1235;
  idle.is_crashing = false;
  idle.frames = {{0x7f0000010000ULL, 0x10000ULL, "/usr/lib/libc.so.6"}};
  info.threads.push_back(idle);

  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("--- --- --- thread 1235"), std::string::npos);
}

TEST(TombstoneFormatterTest, ThreadWithName) {
  MinidumpInfo info = MakeInfo();

  ThreadInfo named;
  named.tid = 9999;
  named.name = "worker";
  named.is_crashing = false;
  info.threads.push_back(named);

  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("worker"), std::string::npos);
}

TEST(TombstoneFormatterTest, UnmappedFrame) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].frames = {{0xbadf00d0ULL, 0, ""}};
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
