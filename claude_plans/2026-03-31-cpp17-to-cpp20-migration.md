# C++17 → C++20 Migration Plan — IMPLEMENTED

## Context

Crashomon currently targets C++17 across all its own C++ code (lib, daemon, tools, bench, test, examples). This migration modernizes the codebase to C++20, adopting `std::format`, `std::span`, `std::ranges`, and designated initializers. The minimum compiler requirement is bumped from GCC ≥ 11 / Clang ≥ 14 to **GCC ≥ 13 / Clang ≥ 16** to enable full `std::format` support.

---

## Phase 1: Build system — flip to C++20, bump compiler minimum

### Files to modify

- **`CMakeLists.txt:9`** — change `set(CMAKE_CXX_STANDARD 17)` → `set(CMAKE_CXX_STANDARD 20)`
- **`CMakeLists.txt:194-195`** — update comment: "our project's global C++17 setting" → "C++20"
- **`examples/CMakeLists.txt:9`** — change `-std=c++17` → `-std=c++20` in `target_compile_options` for `example_segfault`

### Verification

```bash
cmake -B build && cmake --build build && ctest --test-dir build
```

All 90 tests must pass. Crashpad already builds as C++20 internally. Abseil with `ABSL_PROPAGATE_CXX_STD ON` inherits C++20 cleanly. GoogleTest v1.14.0 and Breakpad (custom CMake targets) are unaffected.

---

## Phase 2: Designated initializers

Replace positional aggregate initialization with named fields for clarity.

### Files to modify

- **`daemon/minidump_reader.cpp:84-85`** — `ModuleInfo` push_back uses positional args. Change to:
  ```cpp
  info.modules.push_back({.path = mod->code_file(),
                           .base_address = mod->base_address(),
                           .size = mod->size(),
                           .build_id = mod->debug_identifier()});
  ```

- **`daemon/disk_manager.cpp:43-47`** — `DmpFile` construction via 4 separate assignments. Collapse to:
  ```cpp
  files.push_back(DmpFile{
      .path = entry.path(),
      .size = static_cast<uint64_t>(file_stat.st_size),
      .mtime = file_stat.st_mtime});
  ```

- **`tools/analyze/symbolizer.cpp:111-112`** — `FrameRef` push_back uses positional args. Change to:
  ```cpp
  by_module[frame.module_path].push_back(
      {.tid = thread.tid, .index = frame.index, .offset = frame.module_offset});
  ```

### Verification

Build + tests. Designated initializers enforce field order matching declaration order — all structs have fields in correct order for these initializations.

---

## Phase 3: `std::ranges::sort` with projection

### Files to modify

- **`daemon/disk_manager.cpp:56-59`** — Replace:
  ```cpp
  std::sort(files.begin(), files.end(),
            [](const DmpFile& lhs, const DmpFile& rhs) {
              return lhs.mtime < rhs.mtime;
            });
  ```
  With:
  ```cpp
  std::ranges::sort(files, {}, &DmpFile::mtime);
  ```
  Add `#include <algorithm>` and `#include <ranges>`.

### Verification

Build + tests. Default comparator (`std::ranges::less{}`) + projection `&DmpFile::mtime` = identical ascending sort.

---

## Phase 4: `std::span` for buffer parameter

### Files to modify

- **`daemon/main.cpp:205-207`** — Change `ProcessInotifyEvents` signature from:
  ```cpp
  void ProcessInotifyEvents(std::string_view pending_dir,
                            const crashomon::DiskManagerConfig& prune_cfg,
                            const char* buf, ssize_t count)
  ```
  To:
  ```cpp
  void ProcessInotifyEvents(std::string_view pending_dir,
                            const crashomon::DiskManagerConfig& prune_cfg,
                            std::span<const char> buf)
  ```
  Update the loop body: `buf.size()` for the bound, `buf.data()` for the reinterpret_cast base.

- **`daemon/main.cpp:333`** — Update call site:
  ```cpp
  ProcessInotifyEvents(pending_dir, prune_cfg,
                       std::span<const char>{buf.data(), static_cast<size_t>(num_read)});
  ```
  Add `#include <span>`.

### Verification

Build + tests. Function is in anonymous namespace with single call site — fully self-contained.

---

## Phase 5: `std::format` — replace all `snprintf` calls

Replace all `snprintf`/`std::snprintf` + fixed-size buffer patterns with `std::format`. This eliminates ~15 buffer declarations, removes `PRIx64`/`PRIu32` macros, and provides compile-time format string validation.

### File 1: `daemon/tombstone_formatter.cpp` (4 snprintf sites)

Replace `#include <cinttypes>` and `#include <cstdio>` with `#include <format>`.

- **Lines 37-39** — hex formatting for unknown module frame PC:
  ```cpp
  // Before
  std::array<char, 20> hex{};
  (void)std::snprintf(hex.data(), hex.size(), "%016" PRIx64, frame.pc);
  out << hex.data();

  // After
  out << std::format("{:016x}", frame.pc);
  ```

- **Lines 43-45** — hex formatting for module offset:
  ```cpp
  // After
  out << std::format("{:016x}", frame.module_offset);
  ```

- **Lines 67-69** — fault address formatting:
  ```cpp
  // Before
  std::array<char, 32> hex_addr{};
  (void)std::snprintf(hex_addr.data(), hex_addr.size(), "0x%016" PRIx64, info.fault_addr);
  // ...uses hex_addr.data() twice below

  // After: use a local string
  const auto hex_addr = std::format("0x{:016x}", info.fault_addr);
  // ...use hex_addr (no .data() needed, works with << directly)
  ```

- **Lines 90-93** — register name + value:
  ```cpp
  // Before
  std::array<char, 32> reg_buf{};
  (void)std::snprintf(reg_buf.data(), reg_buf.size(), " %-3s %016" PRIx64,
                      regs[reg_idx].first.c_str(), regs[reg_idx].second);
  out << reg_buf.data();

  // After
  out << std::format(" {:<3s} {:016x}", regs[reg_idx].first, regs[reg_idx].second);
  ```

Also remove `#include <array>` if no longer used in this file (check: `std::array` was only used for snprintf buffers).

### File 2: `tools/analyze/symbolizer.cpp` (11 snprintf sites)

Replace `#include <cinttypes>` and `#include <cstdio>` with `#include <format>`.

- **Line 70-71** — addr2line hex offset:
  ```cpp
  // Before
  char hex[32];
  snprintf(hex, sizeof(hex), " 0x%016" PRIx64, r.offset);
  cmd += hex;

  // After
  cmd += std::format(" 0x{:016x}", r.offset);
  ```

- **Lines 151-156** — process header:
  ```cpp
  // Before
  char hdr[256];
  snprintf(hdr, sizeof(hdr), "pid: %" PRIu32 ", tid: %" PRIu32 ", name: %s  >>> %s <<<\n", ...);
  out << hdr;

  // After
  out << std::format("pid: {}, tid: {}, name: {}  >>> {} <<<\n",
                     tombstone.pid, tombstone.crashing_tid,
                     tombstone.process_name, tombstone.process_name);
  ```

- **Lines 165-177** — signal line (two branches):
  ```cpp
  // After (empty code_name)
  out << std::format("signal {} ({}), fault addr 0x{:016x}\n",
                     tombstone.signal_number, sig_name, tombstone.fault_addr);

  // After (with code_name)
  out << std::format("signal {} ({}), code {} ({}), fault addr 0x{:016x}\n",
                     tombstone.signal_number, sig_name,
                     tombstone.signal_code, code_name, tombstone.fault_addr);
  ```

- **Lines 186-221** — `emit_frames` lambda, 5 snprintf branches all producing frame lines:
  ```cpp
  // Example: symbolicated with source location
  out << std::format("    #{:02d} pc 0x{:016x}  {} ({}) [{}:{}]\n",
                     frame.index, frame.module_offset,
                     frame.module_path.empty() ? "???" : frame.module_path,
                     sym.function, sym.source_file, sym.source_line);
  ```
  Remove the `char line[512]` buffer. Write directly to `out` via `std::format` for each branch.

- **Lines 236-244** — thread separator:
  ```cpp
  // After (no name)
  out << std::format("\n--- --- --- thread {} --- --- ---\n", t.tid);

  // After (with name)
  out << std::format("\n--- --- --- thread {} ({}) --- --- ---\n", t.tid, t.name);
  ```

Remove `#include <cstdio>` from symbolizer.cpp (only used for `snprintf` and `popen`/`pclose`). **Note**: `popen`/`pclose` in `RunCapture()` still need `<cstdio>`, so keep it.

### Verification

Build + all tests. The tombstone formatter has dedicated unit tests in `test/test_tombstone_formatter.cpp` that validate exact output format — these serve as regression tests for the `std::format` migration.

---

## Phase 6: Documentation updates

Update all references from C++17 to C++20 and bump compiler requirements.

### Files to modify

- **`CLAUDE.md`**:
  - Line 9: "C++17 implementation" → "C++20 implementation"
  - Line 26: "C++17, no GNU extensions, no exceptions (`-std=c++17 -fno-exceptions`)" → "C++20, no GNU extensions, no exceptions (`-std=c++20 -fno-exceptions`)"
  - Line 56: "C++17 client library" → "C++20 client library"

- **`README.md`** (7+ references):
  - Line 17: `C++17 (C public API)` → `C++20 (C public API)`
  - Line 19: `C++17` → `C++20` (watcherd row)
  - Line 20: `C++17` → `C++20` (analyze row)
  - Line 33: `GCC ≥ 11 or Clang ≥ 14` → `GCC ≥ 13 or Clang ≥ 16` and `Must support C++17` → `Must support C++20`
  - Line 324: `C++17 implementation` → `C++20 implementation`
  - Line 327: `C++17` → `C++20` (analyze dir description)
  - Line 340: `C++17` → `C++20`

- **`CMakeLists.txt:195`** — comment already covered in Phase 1.

---

## Phase 7: clang-tidy

**No changes required.** The existing `modernize-*` glob automatically picks up C++20 modernize checks. The exclusion list remains appropriate.

---

## What is NOT changing

| Item | Reason |
|------|--------|
| `std::expected` | C++23, not C++20 |
| `absl::Status` / `absl::StatusOr` | Keep as-is; `std::expected` is C++23 |
| `-fno-exceptions` | Orthogonal to C++ standard version |
| `lib/crashomon.h` (C public API) | Pure C header, no C++ features |
| `lib/crashomon.cpp` | No C++20 opportunities identified |
| Concepts, coroutines, modules | No templates or patterns that benefit |
| `std::jthread` | Single detached thread doesn't benefit from cooperative cancellation |
| `[[likely]]` / `[[unlikely]]` | Marginal benefit; compiler handles branch prediction |

---

## Final Verification

After all phases:
```bash
# Full build + tests
cmake -B build && cmake --build build && ctest --test-dir build

# With clang-tidy
cmake -B build -DENABLE_CLANG_TIDY=ON && cmake --build build

# Sanitizer builds (pick one at a time)
cmake -B build -DENABLE_UBSAN=ON && cmake --build build && ctest --test-dir build
cmake -B build -DENABLE_ASAN=ON && cmake --build build && ctest --test-dir build
```

---

## Critical files list

- `CMakeLists.txt` — C++ standard setting + comment
- `examples/CMakeLists.txt` — hardcoded `-std=c++17`
- `daemon/disk_manager.cpp` — designated initializers + ranges::sort
- `daemon/minidump_reader.cpp` — designated initializers
- `daemon/main.cpp` — std::span for inotify buffer
- `daemon/tombstone_formatter.cpp` — std::format (4 snprintf sites)
- `tools/analyze/symbolizer.cpp` — std::format (11 snprintf sites) + designated initializers
- `CLAUDE.md` — documentation (C++17→C++20, compiler bump)
- `README.md` — documentation (C++17→C++20, compiler bump)
