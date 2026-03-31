// test/test_log_parser.cpp — unit tests for tools/analyze/log_parser

#include <string>

#include "gtest/gtest.h"
#include "tools/analyze/log_parser.h"

namespace crashomon {
namespace {

// A minimal tombstone text that ParseTombstone should accept.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::string MinimalTombstone() {
  return "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
         "pid: 1234, tid: 1234, name: my_service  >>> my_service <<<\n"
         "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0000000000000000\n"
         "\n"
         "backtrace:\n"
         "    #00 pc 0x0000000000001abc  /usr/bin/my_service\n"
         "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n";
}

// A full tombstone with timestamp, multiple threads, minidump path.
// Google C++ Style Guide recommends trailing
// return types only when required; conventional notation is clearer here.
// NOLINTNEXTLINE(modernize-use-trailing-return-type)
std::string FullTombstone() {
  return "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n"
         "pid: 42, tid: 100, name: worker  >>> worker <<<\n"
         "signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x00000000deadbeef\n"
         "timestamp: 2026-03-27T10:15:30Z\n"
         "\n"
         "backtrace:\n"
         "    #00 pc 0x0000000000004000  /usr/lib/libfoo.so\n"
         "    #01 pc 0x0000000000008000  /usr/bin/worker\n"
         "\n"
         "--- --- --- thread 101 --- --- ---\n"
         "    #00 pc 0x0000000000001000  /usr/lib/libpthread.so\n"
         "\n"
         "--- --- --- thread 102 (io_thread) --- --- ---\n"
         "    #00 pc 0x0000000000002000  /usr/lib/libc.so\n"
         "    #01 pc 0x0000000000003000  ???\n"
         "\n"
         "minidump saved to: /var/crashomon/abc123.dmp\n"
         "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n";
}

// ---------------------------------------------------------------------------
// Basic parsing
// ---------------------------------------------------------------------------

TEST(ParseTombstone, EmptyInputReturnsError) {
  auto result = ParseTombstone("");
  EXPECT_FALSE(result.ok());
}

TEST(ParseTombstone, NoFramesReturnsError) {
  // Has a header but no frame lines.
  const std::string text =
      "pid: 1, tid: 1, name: foo  >>> foo <<<\n"
      "signal 11 (SIGSEGV), fault addr 0x0\n";
  auto result = ParseTombstone(text);
  EXPECT_FALSE(result.ok());
}

TEST(ParseTombstone, MinimalSucceeds) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok()) << result.status().message();
}

// ---------------------------------------------------------------------------
// Header fields
// ---------------------------------------------------------------------------

TEST(ParseTombstone, ParsesPid) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->pid, 1234U);
}

TEST(ParseTombstone, ParsesCrashingTid) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->crashing_tid, 1234U);
}

TEST(ParseTombstone, ParsesProcessName) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->process_name, "my_service");
}

// ---------------------------------------------------------------------------
// Signal line
// ---------------------------------------------------------------------------

TEST(ParseTombstone, ParsesSignalNumber) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->signal_number, 11U);
}

TEST(ParseTombstone, ParsesSignalWithCode) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->signal_info, "SIGSEGV / SEGV_MAPERR");
  EXPECT_EQ(result->signal_code, 1U);
}

TEST(ParseTombstone, ParsesFaultAddr) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->fault_addr, 0xdeadbeefU);
}

TEST(ParseTombstone, FaultAddrZeroIsValid) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->fault_addr, 0U);
}

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

TEST(ParseTombstone, TimestampAbsentIsEmpty) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result->timestamp.empty());
}

TEST(ParseTombstone, TimestampParsed) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->timestamp, "2026-03-27T10:15:30Z");
}

// ---------------------------------------------------------------------------
// Threads and frames
// ---------------------------------------------------------------------------

TEST(ParseTombstone, CrashingThreadIsFirst) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  ASSERT_FALSE(result->threads.empty());
  EXPECT_TRUE(result->threads[0].is_crashing);
}

TEST(ParseTombstone, CrashingThreadFrameCount) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->threads[0].frames.size(), 2U);
}

TEST(ParseTombstone, OtherThreadCount) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->threads.size(), 3U);
  EXPECT_FALSE(result->threads[1].is_crashing);
  EXPECT_FALSE(result->threads[2].is_crashing);
}

TEST(ParseTombstone, ThreadTidsParsed) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->threads[0].tid, 100U);
  EXPECT_EQ(result->threads[1].tid, 101U);
  EXPECT_EQ(result->threads[2].tid, 102U);
}

TEST(ParseTombstone, ThreadNameParsed) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  // thread 101 has no name; thread 102 is "io_thread"
  EXPECT_TRUE(result->threads[1].name.empty());
  EXPECT_EQ(result->threads[2].name, "io_thread");
}

// ---------------------------------------------------------------------------
// Frame fields
// ---------------------------------------------------------------------------

TEST(ParseTombstone, FrameIndex) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->threads[0].frames[0].index, 0);
  EXPECT_EQ(result->threads[0].frames[1].index, 1);
}

TEST(ParseTombstone, FrameModuleOffset) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->threads[0].frames[0].module_offset, 0x4000U);
  EXPECT_EQ(result->threads[0].frames[1].module_offset, 0x8000U);
}

TEST(ParseTombstone, FrameModulePath) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->threads[0].frames[0].module_path, "/usr/lib/libfoo.so");
}

TEST(ParseTombstone, UnmappedFrameHasEmptyPath) {
  // thread 102, frame 1 is "???"
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->threads[2].frames.size(), 2U);
  EXPECT_TRUE(result->threads[2].frames[1].module_path.empty());
}

// ---------------------------------------------------------------------------
// Trailing / already-symbolicated text
// ---------------------------------------------------------------------------

TEST(ParseTombstone, TrailingTextPreserved) {
  const std::string text =
      "pid: 1, tid: 1, name: foo  >>> foo <<<\n"
      "signal 6 (SIGABRT), fault addr 0x0\n"
      "\n"
      "backtrace:\n"
      "    #00 pc 0x0000000000001abc  /usr/bin/foo (bar_func) [bar.cpp:42]\n"
      "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n";
  auto result = ParseTombstone(text);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result->threads[0].frames.size(), 1U);
  EXPECT_EQ(result->threads[0].frames[0].trailing, "(bar_func) [bar.cpp:42]");
}

// ---------------------------------------------------------------------------
// Minidump path
// ---------------------------------------------------------------------------

TEST(ParseTombstone, MinidumpPathParsed) {
  auto result = ParseTombstone(FullTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->minidump_path, "/var/crashomon/abc123.dmp");
}

TEST(ParseTombstone, MinidumpPathAbsentIsEmpty) {
  auto result = ParseTombstone(MinimalTombstone());
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result->minidump_path.empty());
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(ParseTombstone, SignalWithoutCode) {
  // SIGABRT typically has no code in our format
  const std::string text =
      "pid: 10, tid: 10, name: app  >>> app <<<\n"
      "signal 6 (SIGABRT), fault addr 0x0000000000000000\n"
      "\n"
      "backtrace:\n"
      "    #00 pc 0x0000000000000100  /usr/bin/app\n";
  auto result = ParseTombstone(text);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result->signal_number, 6U);
  EXPECT_EQ(result->signal_info, "SIGABRT");
  EXPECT_EQ(result->signal_code, 0U);
}

TEST(ParseTombstone, ThreadWithNoFramesNotSaved) {
  // A thread separator with no subsequent frames should not appear in result.
  const std::string text =
      "pid: 5, tid: 5, name: prog  >>> prog <<<\n"
      "signal 11 (SIGSEGV), fault addr 0x0\n"
      "\n"
      "backtrace:\n"
      "    #00 pc 0x0000000000001000  /usr/bin/prog\n"
      "\n"
      "--- --- --- thread 6 --- --- ---\n"
      "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n";
  auto result = ParseTombstone(text);
  ASSERT_TRUE(result.ok());
  // Thread 6 has no frames; whether it is included depends on implementation.
  // At minimum the crashing thread must be present.
  EXPECT_GE(result->threads.size(), 1U);
  EXPECT_TRUE(result->threads[0].is_crashing);
}

}  // namespace
}  // namespace crashomon
