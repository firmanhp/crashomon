// Example: triggers SIGSEGV via a deep C++17 call chain.
// Each function is marked noinline so the full stack is visible in crash reports.
// Used for integration testing and fixture .dmp generation.

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace {

struct Record {
  std::uint32_t id;
  std::uint32_t value;
};

// Leaf: writes to a field. Crashes when ptr is null.
[[gnu::noinline]] void WriteField(volatile std::uint32_t* ptr,
                                  std::uint32_t value) {
  *ptr = value;  // SIGSEGV when ptr is null
}

// Updates the value field of a record.
[[gnu::noinline]] void UpdateRecord(Record* rec, std::uint32_t new_value) {
  // rec is intentionally null — triggers the crash in WriteField.
  WriteField(&rec->value, new_value);  // NOLINT(clang-analyzer-core.NullDereference)
}

// Processes one entry from a record buffer.
[[gnu::noinline]] void ProcessEntry(Record* rec) {
  UpdateRecord(rec, 0xdeadbeefU);
}

// Validates and dispatches records from a named buffer.
[[gnu::noinline]] void DispatchRecords(std::string_view name,
                                       Record* records,
                                       std::size_t count) {
  std::fputs("  dispatching record(s) from '", stdout);
  std::fwrite(name.data(), 1, name.size(), stdout);
  std::fputs("'\n", stdout);
  for (std::size_t i = 0; i < count; ++i) {
    ProcessEntry(&records[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  }
}

// Simulates a top-level processing pipeline.
[[gnu::noinline]] void RunPipeline() {
  // Deliberately pass nullptr as the record array to trigger the crash.
  DispatchRecords("sensor-feed", nullptr, 1);
}

}  // namespace

int main() {
  std::puts("crashomon example: running pipeline (will crash)...");
  RunPipeline();
  return 0;
}
