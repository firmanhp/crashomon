// bench/bench_tombstone_formatter.cpp — microbenchmarks for FormatTombstone.
//
// Run with: ./build/bench/crashomon_bench --benchmark_filter=BM_FormatTombstone

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "benchmark/benchmark.h"
#include "tombstone/minidump_reader.h"
#include "tombstone/tombstone_formatter.h"

namespace crashomon {
namespace {

// ── Helpers ──────────────────────────────────────────────────────────────────

FrameInfo MakeFrame(int idx) {
  return {
      .pc = static_cast<uint64_t>(0x5555'0000'0000ULL) + static_cast<uint64_t>(idx) * uint64_t{0x100},
      .module_offset = static_cast<uint64_t>(idx) * uint64_t{0x100},
      .module_path = "/usr/bin/bench_binary",
  };
}

ThreadInfo MakeCrashingThread(int num_frames) {
  ThreadInfo t;
  t.tid = 1;
  t.is_crashing = true;
  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers) — sequential register values
  t.registers = {
      {"rax", 0x0}, {"rbx", 0x1}, {"rcx", 0x2}, {"rdx", 0x3}, {"rsi", 0x4},  {"rdi", 0x5},
      {"rbp", 0x6}, {"rsp", 0x7}, {"r8", 0x8},  {"r9", 0x9},  {"r10", 0xa},  {"r11", 0xb},
      {"r12", 0xc}, {"r13", 0xd}, {"r14", 0xe}, {"r15", 0xf}, {"rip", 0x10},
  };
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers)
  for (int i = 0; i < num_frames; ++i) {
    t.frames.push_back(MakeFrame(i));
  }
  return t;
}

ThreadInfo MakeNonCrashingThread(uint32_t tid) {
  ThreadInfo t;
  t.tid = tid;
  t.is_crashing = false;
  t.frames.push_back(MakeFrame(0));
  return t;
}

MinidumpInfo MakeInfo(int num_threads, int frames_per_crashing_thread) {
  MinidumpInfo info;
  info.pid = 1234;
  info.crashing_tid = 1;
  info.process_name = "bench_binary";
  info.signal_info = "SIGSEGV / SEGV_MAPERR";
  info.signal_number = 11;
  info.signal_code = 1;
  info.fault_addr = 0xdead'beef'0000'0000ULL;
  info.timestamp = "2025-01-01T00:00:00Z";
  info.minidump_path = "/var/crashomon/bench.dmp";
  info.modules = {{.path = "/usr/bin/bench_binary",
                   .base_address = 0x5555'0000'0000ULL,
                   .size = 0x1000,
                   .build_id = "AABBCCDD0"}};

  info.threads.push_back(MakeCrashingThread(frames_per_crashing_thread));
  for (int i = 1; i < num_threads; ++i) {
    info.threads.push_back(MakeNonCrashingThread(static_cast<uint32_t>(i + 1)));
  }
  return info;
}

// ── Benchmarks ────────────────────────────────────────────────────────────────

// Single crashing thread with a typical 10-frame stack.
void BM_FormatTombstone_SingleThread(benchmark::State& state) {
  const MinidumpInfo info = MakeInfo(/*num_threads=*/1, /*frames=*/10);
  for (auto _ : state) {
    benchmark::DoNotOptimize(FormatTombstone(info));
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_FormatTombstone_SingleThread);

// Scale by thread count: 1, 4, 8, 16 threads (each with 5 frames).
void BM_FormatTombstone_MultiThread(benchmark::State& state) {
  const int num_threads = static_cast<int>(state.range(0));
  const MinidumpInfo info = MakeInfo(num_threads, /*frames=*/5);
  for (auto _ : state) {
    benchmark::DoNotOptimize(FormatTombstone(info));
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_FormatTombstone_MultiThread)->Arg(1)->Arg(4)->Arg(8)->Arg(16);

// Scale by crashing-thread stack depth: 10, 50, 100, 500 frames.
void BM_FormatTombstone_DeepStack(benchmark::State& state) {
  const int num_frames = static_cast<int>(state.range(0));
  const MinidumpInfo info = MakeInfo(/*num_threads=*/1, num_frames);
  for (auto _ : state) {
    benchmark::DoNotOptimize(FormatTombstone(info));
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_FormatTombstone_DeepStack)->Arg(10)->Arg(50)->Arg(100)->Arg(500);

}  // namespace
}  // namespace crashomon
