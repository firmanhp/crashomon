# Extract Tombstone Library & Add Raw Tombstone Mode to crashomon-analyze [IMPLEMENTED]

## Context

The daemon's `ProcessNewMinidump` is disabled because Breakpad's `MinidumpProcessor` crashes the daemon under high crash load (see stress test: `test/stress_test.sh`). The minidump reader and tombstone formatter are currently compiled directly into the daemon binary, making it impossible to test them in isolation.

This refactoring extracts those modules into a standalone static library (`crashomon_tombstone`) and adds a raw tombstone mode to `crashomon-analyze`, so the same code can run independently of the daemon. This sets the stage for a follow-up crash investigation.

---

## Architecture

```
crashomon/
  tombstone/                    <- NEW: static library
    minidump_reader.h/cpp       <- moved from daemon/
    tombstone_formatter.h/cpp   <- moved from daemon/
    CMakeLists.txt

  daemon/                       <- links crashomon_tombstone
    main.cpp
    disk_manager.h/cpp
    CMakeLists.txt              <- updated

  tools/
    analyze/                    <- links crashomon_tombstone, gains new mode
      main.cpp                  <- new mode: --minidump alone -> raw tombstone
      CMakeLists.txt            <- updated
      ...

  test/                         <- links crashomon_tombstone
    CMakeLists.txt              <- updated
```

---

## Implementation

### Step 1: Create `tombstone/` static library

Create `tombstone/CMakeLists.txt`:

```cmake
add_library(crashomon_tombstone STATIC
  minidump_reader.cpp
  tombstone_formatter.cpp
)

target_link_libraries(crashomon_tombstone
  PRIVATE
    crashomon_warnings
    breakpad_processor
    absl::status
    absl::statusor
)

target_include_directories(crashomon_tombstone
  PUBLIC
    ${CMAKE_SOURCE_DIR}
)

# SYSTEM include workarounds (same as daemon/CMakeLists.txt)
if(abseil_SOURCE_DIR)
  target_include_directories(crashomon_tombstone SYSTEM PRIVATE "${abseil_SOURCE_DIR}")
endif()
if(breakpad_SOURCE_DIR)
  target_include_directories(crashomon_tombstone SYSTEM PRIVATE
    "${breakpad_SOURCE_DIR}/src")
endif()

set_target_properties(crashomon_tombstone PROPERTIES
  CXX_CLANG_TIDY "${CRASHOMON_CLANG_TIDY}"
)
```

### Step 2: Move files from `daemon/` to `tombstone/`

Move these files (git mv):
- `daemon/minidump_reader.h` -> `tombstone/minidump_reader.h`
- `daemon/minidump_reader.cpp` -> `tombstone/minidump_reader.cpp`
- `daemon/tombstone_formatter.h` -> `tombstone/tombstone_formatter.h`
- `daemon/tombstone_formatter.cpp` -> `tombstone/tombstone_formatter.cpp`

Update include paths inside the moved files:
- `tombstone/minidump_reader.cpp`: `#include "daemon/minidump_reader.h"` -> `#include "tombstone/minidump_reader.h"`
- `tombstone/tombstone_formatter.h`: `#include "daemon/minidump_reader.h"` -> `#include "tombstone/minidump_reader.h"`
- `tombstone/tombstone_formatter.cpp`: `#include "daemon/tombstone_formatter.h"` -> `#include "tombstone/tombstone_formatter.h"`

Update header comments (remove "internal to crashomon-watcherd" wording).

### Step 3: Update `daemon/CMakeLists.txt`

Remove `tombstone_formatter.cpp` and `minidump_reader.cpp` from `add_executable` sources.

Add `crashomon_tombstone` to `target_link_libraries`.

Remove `breakpad_processor`, `absl::status`, `absl::statusor` from daemon's direct link deps (now transitive via the library). Keep `crashpad_handler_lib`, `crashpad_tools`, `crashpad_client`, `Threads::Threads`.

### Step 4: Update `daemon/main.cpp`

Update includes:
- `#include "daemon/minidump_reader.h"` -> `#include "tombstone/minidump_reader.h"`
- `#include "daemon/tombstone_formatter.h"` -> `#include "tombstone/tombstone_formatter.h"`

### Step 5: Update `tools/analyze/main.cpp`

Add new includes:
```cpp
#include "tombstone/minidump_reader.h"
#include "tombstone/tombstone_formatter.h"
```

Add `ModeRawTombstone()` function:
```cpp
int ModeRawTombstone(const std::string& dump) {
  auto info_or = crashomon::ReadMinidump(dump);
  if (!info_or.ok()) {
    // print error to stderr
    return 1;
  }
  // print FormatTombstone(*info_or) to stdout
  return 0;
}
```

Update dispatch in `main()` — add after the three existing mode checks, before the "no valid mode" error:
```cpp
if (!minidump.empty()) {
  return ModeRawTombstone(minidump);
}
```

Update `Usage()` to document the new mode:
```
  crashomon-analyze --minidump <file.dmp>
      Print raw (unsymbolicated) Android-style tombstone from a minidump.
```

### Step 6: Update `tools/analyze/CMakeLists.txt`

Add `crashomon_tombstone` to `target_link_libraries`.

### Step 7: Update `test/CMakeLists.txt`

Remove from `target_sources`:
- `${CMAKE_SOURCE_DIR}/daemon/tombstone_formatter.cpp`
- `${CMAKE_SOURCE_DIR}/daemon/minidump_reader.cpp`

Add `crashomon_tombstone` to `target_link_libraries`.

### Step 8: Update test includes

- `test/test_minidump_reader.cpp`: `#include "daemon/minidump_reader.h"` -> `#include "tombstone/minidump_reader.h"`
- `test/test_tombstone_formatter.cpp`: `#include "daemon/tombstone_formatter.h"` -> `#include "tombstone/tombstone_formatter.h"`

### Step 9: Update root `CMakeLists.txt`

Add `add_subdirectory(tombstone)` before `add_subdirectory(daemon)` so the library target is available when the daemon's CMakeLists.txt runs.

---

## Files summary

### New files
| File | Purpose |
|------|---------|
| `tombstone/CMakeLists.txt` | Static library `crashomon_tombstone` |

### Moved files (git mv)
| From | To |
|------|-----|
| `daemon/minidump_reader.h` | `tombstone/minidump_reader.h` |
| `daemon/minidump_reader.cpp` | `tombstone/minidump_reader.cpp` |
| `daemon/tombstone_formatter.h` | `tombstone/tombstone_formatter.h` |
| `daemon/tombstone_formatter.cpp` | `tombstone/tombstone_formatter.cpp` |

### Modified files
| File | Change |
|------|--------|
| `daemon/CMakeLists.txt` | Remove moved sources, link `crashomon_tombstone` |
| `daemon/main.cpp` | Update includes to `tombstone/` |
| `tools/analyze/main.cpp` | Add `ModeRawTombstone`, update dispatch + usage |
| `tools/analyze/CMakeLists.txt` | Link `crashomon_tombstone` |
| `test/CMakeLists.txt` | Replace `target_sources` with library link |
| `test/test_minidump_reader.cpp` | Update include path |
| `test/test_tombstone_formatter.cpp` | Update include path |
| `CMakeLists.txt` (root) | Add `add_subdirectory(tombstone)` |

---

## Verification

```bash
# Build
cmake -B build && cmake --build build

# Unit tests (90 tests)
ctest --test-dir build

# Verify the new mode works (requires a .dmp fixture)
./build/crashomon-analyze --minidump test/fixtures/segfault.dmp

# Integration test (existing)
test/integration_test.sh
```
