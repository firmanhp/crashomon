// test/test_tombstone_formatter.cpp — unit tests for FormatTombstone

#include <cstddef>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"
#include "daemon/tombstone/minidump_reader.h"
#include "daemon/tombstone/tombstone_formatter.h"

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
MinidumpInfo MakeInfo() {
  MinidumpInfo
      info;  // NOLINT(misc-include-cleaner) — MinidumpInfo comes via tombstone/minidump_reader.h
             // which is included; false positive from include-cleaner.
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
      crashing;  // NOLINT(misc-include-cleaner) — ThreadInfo comes via tombstone/minidump_reader.h
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
  // build_id is set on each frame so the formatter can emit (BuildId: ...) directly.
  crashing.frames = {
      {kFrame0Pc, kFrame0Offset, "/usr/bin/my_service", "9c1e3ae2f0aabb00"},
      {kFrame1Pc, kFrame1Offset, "/usr/bin/my_service", "9c1e3ae2f0aabb00"},
      {kFrame2Pc, kFrame2Offset, "/usr/lib/libc.so.6", "7a4f0b11d3ccdd00"},
  };
  info.threads.push_back(crashing);

  ModuleInfo  // NOLINT(misc-include-cleaner) — false positive from include-cleaner
      svc_mod;
  svc_mod.path = "/usr/bin/my_service";
  svc_mod.build_id = "9c1e3ae2f0aabb00";
  info.modules.push_back(svc_mod);

  ModuleInfo libc_mod;
  libc_mod.path = "/usr/lib/libc.so.6";
  libc_mod.build_id = "7a4f0b11d3ccdd00";
  info.modules.push_back(libc_mod);

  return info;
}

TEST(TombstoneFormatterTest, ContainsSeparators) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_NE(tomb.find("CRASH DETECTED"), std::string::npos);
  EXPECT_NE(tomb.find("END OF CRASH"), std::string::npos);
  // Both separators contain the star pattern.
  const size_t first = tomb.find("*** *** ***");
  const size_t last = tomb.rfind("*** *** ***");
  EXPECT_NE(first, last);
}

TEST(TombstoneFormatterTest, ContainsProcessAndThreadHeader) {
  auto tomb = FormatTombstone(MakeInfo());
  // New header format: process: <name> (pid N)  thread: <name> (tid N)
  EXPECT_NE(tomb.find("process: my_service (pid 1234)"), std::string::npos);
  // Thread name falls back to process name when crashing thread has no name.
  EXPECT_NE(tomb.find("thread: my_service (tid 1234)"), std::string::npos);
}

TEST(TombstoneFormatterTest, CrashingThreadNameShownInHeader) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].name = "main-thread";
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("thread: main-thread (tid 1234)"), std::string::npos);
  // Process name still shows the binary name.
  EXPECT_NE(tomb.find("process: my_service (pid 1234)"), std::string::npos);
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

TEST(TombstoneFormatterTest, RegistersNotPrinted) {
  auto tomb = FormatTombstone(MakeInfo());
  // Registers are not actionable in journald — they must not appear in the tombstone.
  EXPECT_EQ(tomb.find("rax"), std::string::npos);
  // rip value 0x10 → "0000000000000010" must not appear as a register line.
  EXPECT_EQ(tomb.find("rip"), std::string::npos);
}

TEST(TombstoneFormatterTest, ContainsPcLine) {
  auto tomb = FormatTombstone(MakeInfo());
  // Only frames[0] is printed as a bare "pc <offset>  <module> (BuildId: ...)" line.
  EXPECT_NE(tomb.find("/usr/bin/my_service"), std::string::npos);
  EXPECT_NE(tomb.find("BuildId: 9c1e3ae2f0aabb00"), std::string::npos);
  // frames[1] and frames[2] are not printed.
  EXPECT_EQ(tomb.find("/usr/lib/libc.so.6"), std::string::npos);
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
  idle.frames = {{kThread2Frame0Pc, kThread2Frame0Offset, "/usr/lib/libc.so.6", ""}};
  info.threads.push_back(idle);

  // Non-crashing threads are NOT printed — only the crashing thread is shown.
  auto tomb = FormatTombstone(info);
  EXPECT_EQ(tomb.find("tid: 1235"), std::string::npos);
}

TEST(TombstoneFormatterTest, ThreadWithName) {
  MinidumpInfo info = MakeInfo();

  ThreadInfo named;
  named.tid = kTestTid3;
  named.name = "worker";
  named.is_crashing = false;
  info.threads.push_back(named);

  // Non-crashing threads are NOT printed — only the crashing thread is shown.
  auto tomb = FormatTombstone(info);
  EXPECT_EQ(tomb.find("worker"), std::string::npos);
}

TEST(TombstoneFormatterTest, UnmappedFrame) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].frames = {{kTestUnmappedPc, 0, "", ""}};
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("???"), std::string::npos);
}

TEST(TombstoneFormatterTest, EmptyRegisters) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].registers.clear();
  auto tomb = FormatTombstone(info);
  // PC line still emitted even without registers (frame is pre-populated in test data).
  EXPECT_NE(tomb.find("/usr/bin/my_service"), std::string::npos);
}

TEST(TombstoneFormatterTest, FaultAddrZero) {
  MinidumpInfo info = MakeInfo();
  info.fault_addr = 0;
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("0000000000000000"), std::string::npos);
}

// ── BuildId emission ─────────────────────────────────────────────────────────

TEST(TombstoneFormatterTest, MappedFrameCarriesBuildId) {
  auto tomb = FormatTombstone(MakeInfo());
  // Only frames[0] is printed; its build ID should appear.
  EXPECT_NE(tomb.find("(BuildId: 9c1e3ae2f0aabb00)"), std::string::npos);
  // frames[2] (/usr/lib/libc.so.6) is not printed — its build ID must not appear.
  EXPECT_EQ(tomb.find("(BuildId: 7a4f0b11d3ccdd00)"), std::string::npos);
}

TEST(TombstoneFormatterTest, UnmappedFrameHasNoBuildId) {
  MinidumpInfo info = MakeInfo();
  info.threads[0].frames = {{kTestUnmappedPc, 0, "", ""}};
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("???"), std::string::npos);
  EXPECT_EQ(tomb.find("BuildId"), std::string::npos);
}

TEST(TombstoneFormatterTest, EmptyBuildIdNotEmitted) {
  MinidumpInfo info = MakeInfo();
  // Clear the build ID on the printed frame — formatter must not emit "(BuildId: )".
  info.threads[0].frames[0].build_id.clear();
  auto tomb = FormatTombstone(info);
  EXPECT_EQ(tomb.find("BuildId"), std::string::npos);
}

TEST(TombstoneFormatterTest, FrameWithEmptyBuildIdHasNoSuffix) {
  MinidumpInfo info = MakeInfo();
  // Replace the crashing frame with one that has no build ID.
  info.threads[0].frames = {{kFrame0Pc, kFrame0Offset, "/usr/lib/libunknown.so", ""}};
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("/usr/lib/libunknown.so"), std::string::npos);
  EXPECT_EQ(tomb.find("BuildId"), std::string::npos);
}

// ── Probable cause ────────────────────────────────────────────────────────────

TEST(TombstoneFormatterTest, ProbableCauseNullDeref) {
  MinidumpInfo info = MakeInfo();
  info.signal_number = kTestSigSegv;
  info.fault_addr = 0;
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("probable cause: null pointer dereference"), std::string::npos);
}

TEST(TombstoneFormatterTest, ProbableCauseNullDerefWithOffset) {
  MinidumpInfo info = MakeInfo();
  info.signal_number = kTestSigSegv;
  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  info.fault_addr = 0x40;  // small offset — still in null page
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("probable cause: null pointer dereference — access at offset"),
            std::string::npos);
}

TEST(TombstoneFormatterTest, ProbableCauseAbort) {
  MinidumpInfo info = MakeInfo();
  info.signal_number = kTestSigAbrt;
  info.signal_info = "SIGABRT";
  info.signal_code = 0;
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("probable cause: abort()"), std::string::npos);
}

TEST(TombstoneFormatterTest, NoProbableCauseAmbiguousSIGSEGV) {
  MinidumpInfo info = MakeInfo();
  // fault_addr well outside null page, not near sp (rsp=0x7), not equal to pc (rip=0x10).
  info.fault_addr = kTestFaultAddr;  // 0xdeadbeef00000000
  auto tomb = FormatTombstone(info);
  EXPECT_EQ(tomb.find("probable cause"), std::string::npos);
}

// ── Abort message ─────────────────────────────────────────────────────────────

TEST(TombstoneFormatterTest, AbortMessageOnlyPrintsAbortMessageLine) {
  MinidumpInfo info = MakeInfo();
  info.abort_message = "assertion failed: 'count >= 0' (counter.cpp:17, tick())";
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("Abort message: 'assertion failed: 'count >= 0' (counter.cpp:17, tick())'"),
            std::string::npos);
}

TEST(TombstoneFormatterTest, AbortMessageWithTerminateTypeCombinesBoth) {
  MinidumpInfo info = MakeInfo();
  info.abort_message = "unhandled C++ exception";
  info.terminate_type = "std::logic_error";
  auto tomb = FormatTombstone(info);
  EXPECT_NE(tomb.find("Abort message: 'std::logic_error: unhandled C++ exception'"),
            std::string::npos);
}

TEST(TombstoneFormatterTest, EmptyAbortMessageProducesNoAbortMessageLine) {
  auto tomb = FormatTombstone(MakeInfo());
  EXPECT_EQ(tomb.find("Abort message:"), std::string::npos);
}

TEST(TombstoneFormatterTest, AbortMessageAppearsAfterSignalLine) {
  MinidumpInfo info = MakeInfo();
  info.abort_message = "test msg";
  auto tomb = FormatTombstone(info);
  const size_t sig_pos = tomb.find("signal 11");
  const size_t abort_pos = tomb.find("Abort message:");
  ASSERT_NE(sig_pos, std::string::npos);
  ASSERT_NE(abort_pos, std::string::npos);
  EXPECT_GT(abort_pos, sig_pos);
}

TEST(TombstoneFormatterTest, AbortMessageAppearsBeforeProbableCause) {
  // SIGABRT always produces a probable cause line; verify Abort message comes first.
  MinidumpInfo info = MakeInfo();
  info.signal_number = kTestSigAbrt;
  info.signal_info = "SIGABRT";
  info.signal_code = 0;
  info.fault_addr = 0;
  info.abort_message = "assertion failed: 'x > 0'";
  auto tomb = FormatTombstone(info);
  const size_t abort_pos = tomb.find("Abort message:");
  const size_t cause_pos = tomb.find("probable cause:");
  ASSERT_NE(abort_pos, std::string::npos);
  ASSERT_NE(cause_pos, std::string::npos);
  EXPECT_LT(abort_pos, cause_pos);
}

}  // namespace
}  // namespace crashomon
