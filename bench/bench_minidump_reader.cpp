// bench/bench_minidump_reader.cpp — microbenchmarks for ReadMinidump.
//
// Run with: ./build/bench/crashomon_bench --benchmark_filter=BM_ReadMinidump
//
// Fixture location: CRASHOMON_FIXTURES_DIR env var, then build/test/fixtures/,
// then test/fixtures/. Benchmark is skipped (marked as SKIPPED) when no fixture
// is found, so it never fails the build.

#include <cstdlib>
#include <filesystem>
#include <string>

#include "absl/status/statusor.h"
#include "benchmark/benchmark.h"
#include "tombstone/minidump_reader.h"

namespace crashomon {
namespace {

// Returns the path to the segfault fixture, or empty string if not found.
std::string FindFixture() {
  // Check CRASHOMON_FIXTURES_DIR first (set by ctest / developer).
  const char* env = std::getenv("CRASHOMON_FIXTURES_DIR");
  if (env != nullptr && *env != '\0') {
    std::filesystem::path p = std::filesystem::path(env) / "segfault.dmp";
    if (std::filesystem::exists(p)) {
      return p.string();
    }
  }
  // Fall back to the CMake build tree location set by gen_synthetic_fixtures.
  const std::filesystem::path build_fixture =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "build" / "test" / "fixtures" /
      "segfault.dmp";
  if (std::filesystem::exists(build_fixture)) {
    return build_fixture.string();
  }
  // Last resort: relative path from repo root.
  std::filesystem::path repo_fixture = std::filesystem::path("test") / "fixtures" / "segfault.dmp";
  if (std::filesystem::exists(repo_fixture)) {
    return repo_fixture.string();
  }
  return {};
}

// Parses a minidump file repeatedly.
void BM_ReadMinidump(benchmark::State& state) {
  const std::string fixture = FindFixture();
  if (fixture.empty()) {
    state.SkipWithMessage("segfault.dmp fixture not found; set CRASHOMON_FIXTURES_DIR");
    return;
  }
  for (auto _ : state) {
    absl::StatusOr<MinidumpInfo> result = ReadMinidump(fixture);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_ReadMinidump);

}  // namespace
}  // namespace crashomon
