// test/gen_synthetic_fixtures.cpp — generates synthetic .dmp fixture files.
//
// Writes three minimal Linux/AMD64 minidumps to <output_dir>/:
//   segfault.dmp    — single thread, SIGSEGV
//   abort.dmp       — single thread, SIGABRT
//   multithread.dmp — three threads, SIGSEGV on thread 1
//
// Usage: gen_synthetic_fixtures <output_dir>
//
// The resulting .dmp files satisfy the assertions in test_minidump_reader.cpp
// without requiring a running crashomon-watcherd daemon. They are generated at
// build time by CMake so `ctest` sees 0 skipped fixture tests.
//
// Design notes:
// - We use SynthMinidump for the overall dump assembly.
// - AMD64 context is appended as raw bytes via Context(dump) + Append() since
//   SynthMinidump::Context only has built-in constructors for x86/ARM/MIPS.
// - System info is written as a raw Stream instead of SystemInfo because
//   SynthMinidump::SystemInfo writes only 48 bytes for non-x86/ARM architectures,
//   but sizeof(MDRawSystemInfo)=56 (MDCPUInformation union is 24 bytes, sized to
//   x86_cpu_info). Writing raw bytes lets us hit the exact expected size.

#include <fstream>
#include <iostream>
#include <string>

// Processor headers pull in minidump_format.h → breakpad_types.h (defines
// uint128_struct). Include them BEFORE minidump_cpu_amd64.h which uses that type.
#include "google_breakpad/processor/minidump.h"
#include "google_breakpad/common/minidump_cpu_amd64.h"
#include "google_breakpad/common/minidump_format.h"
#include "processor/synth_minidump.h"

using google_breakpad::SynthMinidump::Context;
using google_breakpad::SynthMinidump::Dump;
using google_breakpad::SynthMinidump::Exception;
using google_breakpad::SynthMinidump::Memory;
using google_breakpad::SynthMinidump::Module;
using google_breakpad::SynthMinidump::Stream;
using google_breakpad::SynthMinidump::String;
using google_breakpad::SynthMinidump::Thread;
using google_breakpad::test_assembler::kLittleEndian;

// ── Constants ─────────────────────────────────────────────────────────────────

// A realistic-looking RIP value inside our fake module (base 0x400000, size 64K).
static constexpr uint64_t kFakeRip  = 0x401234ULL;
static constexpr uint64_t kFakeRsp1 = 0x7fff00001000ULL;
static constexpr uint64_t kFakeRsp2 = 0x7ffe00001000ULL;
static constexpr uint64_t kFakeRsp3 = 0x7ffd00001000ULL;
static constexpr uint64_t kModBase  = 0x400000ULL;
static constexpr uint32_t kModSize  = 0x10000U;
static constexpr size_t   kStackLen = 256;

// Stable non-epoch timestamp: 2025-01-01T00:00:00Z.
static constexpr uint32_t kTimestamp = 1735689600U;

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool WriteFile(const std::string& path, const std::string& contents) {
  std::ofstream out(path, std::ios::binary);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(out);
}

// Appends raw bytes of an MDRawContextAMD64 struct to an empty Context.
static void AppendAMD64Context(Context& ctx, uint64_t rip, uint64_t rsp) {
  MDRawContextAMD64 raw{};
  raw.context_flags = MD_CONTEXT_AMD64;
  raw.rip = rip;
  raw.rsp = rsp;
  ctx.Append(reinterpret_cast<const uint8_t*>(&raw), sizeof(raw));
}

// Returns a raw MD_SYSTEM_INFO_STREAM with Linux/AMD64 platform/architecture.
//
// SynthMinidump::SystemInfo writes only 48 bytes for non-x86/ARM CPUs, but
// MDRawSystemInfo is always 56 bytes (MDCPUInformation union = 24 bytes).
// We serialize the complete struct directly to avoid the size mismatch.
static Stream MakeLinuxAMD64SystemInfo(const Dump& dump) {
  Stream sysinfo_stream(dump, MD_SYSTEM_INFO_STREAM);
  MDRawSystemInfo raw{};
  raw.processor_architecture = MD_CPU_ARCHITECTURE_AMD64;
  raw.platform_id = MD_OS_LINUX;
  raw.number_of_processors = 1;
  static_assert(sizeof(raw) == 56, "MDRawSystemInfo must be 56 bytes");
  sysinfo_stream.Append(reinterpret_cast<const uint8_t*>(&raw), sizeof(raw));
  return sysinfo_stream;
}

// ── Fixture generators ────────────────────────────────────────────────────────

// A single-thread Linux/AMD64 minidump that crashes with the given signal.
static bool WriteOneThreadFixture(const std::string& path,
                                  uint32_t signal_code,
                                  const char* module_path) {
  Dump dump(MD_NORMAL, kLittleEndian, MD_HEADER_VERSION, kTimestamp);

  Memory stack(dump, kFakeRsp1);
  stack.Append(kStackLen, 0);

  Context ctx(dump);
  AppendAMD64Context(ctx, kFakeRip, kFakeRsp1);

  Thread thread(dump, 1, stack, ctx);

  // First module = process name.
  String mod_name(dump, module_path);
  Module module(dump, kModBase, kModSize, mod_name);

  // Exception: signal number as exception code, thread 1 as the crashing thread.
  Exception exc(dump, ctx, /*thread_id=*/1, signal_code,
                /*exception_flags=*/0, /*exception_address=*/kFakeRip);

  Stream sysinfo = MakeLinuxAMD64SystemInfo(dump);

  dump.Add(&stack);
  dump.Add(&ctx);
  dump.Add(&thread);
  dump.Add(&mod_name);
  dump.Add(&module);
  dump.Add(&sysinfo);
  dump.Add(&exc);
  dump.Finish();

  std::string contents;
  if (!dump.GetContents(&contents)) {
    std::cerr << "GetContents failed for " << path << "\n";
    return false;
  }
  return WriteFile(path, contents);
}

// Three-thread Linux/AMD64 minidump: thread 1 crashes with SIGSEGV, threads
// 2 and 3 are non-crashing.
static bool WriteMultithreadFixture(const std::string& path) {
  Dump dump(MD_NORMAL, kLittleEndian, MD_HEADER_VERSION, kTimestamp);

  Memory stack1(dump, kFakeRsp1);
  stack1.Append(kStackLen, 0);
  Memory stack2(dump, kFakeRsp2);
  stack2.Append(kStackLen, 0);
  Memory stack3(dump, kFakeRsp3);
  stack3.Append(kStackLen, 0);

  Context ctx1(dump);
  AppendAMD64Context(ctx1, kFakeRip, kFakeRsp1);
  Context ctx2(dump);
  AppendAMD64Context(ctx2, kFakeRip + 0x1000, kFakeRsp2);
  Context ctx3(dump);
  AppendAMD64Context(ctx3, kFakeRip + 0x2000, kFakeRsp3);

  Thread thread1(dump, 1, stack1, ctx1);
  Thread thread2(dump, 2, stack2, ctx2);
  Thread thread3(dump, 3, stack3, ctx3);

  String mod_name(dump, "/usr/bin/crashomon_test_multithread");
  Module module(dump, kModBase, kModSize, mod_name);

  // Thread 1 is the crashing thread.
  Exception exc(dump, ctx1, /*thread_id=*/1, /*exception_code=*/11,
                /*exception_flags=*/0, /*exception_address=*/kFakeRip);

  Stream sysinfo = MakeLinuxAMD64SystemInfo(dump);

  dump.Add(&stack1);
  dump.Add(&stack2);
  dump.Add(&stack3);
  dump.Add(&ctx1);
  dump.Add(&ctx2);
  dump.Add(&ctx3);
  dump.Add(&thread1);
  dump.Add(&thread2);
  dump.Add(&thread3);
  dump.Add(&mod_name);
  dump.Add(&module);
  dump.Add(&sysinfo);
  dump.Add(&exc);
  dump.Finish();

  std::string contents;
  if (!dump.GetContents(&contents)) {
    std::cerr << "GetContents failed for " << path << "\n";
    return false;
  }
  return WriteFile(path, contents);
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: gen_synthetic_fixtures <output_dir>\n";
    return 1;
  }
  const std::string dir = argv[1];

  bool ok = true;
  ok &= WriteOneThreadFixture(dir + "/segfault.dmp",
                               /*signal=*/11,
                               "/usr/bin/crashomon_test_segfault");
  ok &= WriteOneThreadFixture(dir + "/abort.dmp",
                               /*signal=*/6,
                               "/usr/bin/crashomon_test_abort");
  ok &= WriteMultithreadFixture(dir + "/multithread.dmp");

  if (!ok) {
    std::cerr << "gen_synthetic_fixtures: one or more fixtures failed\n";
    return 1;
  }
  std::cout << "Generated fixtures in " << dir << "\n";
  return 0;
}
