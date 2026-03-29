# Plan: Replace sentry-native with Direct Crashpad

## Context

Crashomon uses sentry-native 0.7.17 solely as a wrapper to spawn Crashpad's out-of-process
crash handler. Of sentry-native's feature set, crashomon uses ~20%: init, close, and
write-only tags/breadcrumbs that are **never read back** by the daemon, web UI, or analysis
tools. Everything else (transport, sessions, traces, DSN, envelopes) is explicitly disabled
or faked with hacks (dummy DSN, curl stub, C_EXTENSIONS workaround).

This creates a "wrapper of a wrapper" perception that undermines the project's credibility
for open-source release. Replacing sentry-native with direct Crashpad usage removes the
unnecessary abstraction layer, eliminates several build hacks, and results in a cleaner,
more honest dependency story.

**Scope**: Build system + one source file + one build script. Daemon, tools, web UI, and
tests are unaffected.

---

## Approach

Use **Google's official Crashpad** as a **git submodule**, built separately with its native
**GN + Ninja** build system. CMake consumes the prebuilt artifacts via IMPORTED targets.

Why this approach:
- **No sentry ecosystem dependency at all** — clean provenance story
- **Crashpad built with its own toolchain** — no fragile CMake wrappers for a GN project
- **We don't modify Crashpad source** — submodule is read-only, built as-is
- **One script to clone + build** — documented in README, run once before `cmake`

---

## New files

### 1. `scripts/build-crashpad.sh` — Fetch deps + build Crashpad

```bash
#!/usr/bin/env bash
# Fetches Crashpad's dependencies (mini_chromium, lss, etc.) via gclient
# and builds crashpad_handler + client libraries with GN/Ninja.
#
# Prerequisites: depot_tools in PATH (provides gclient, gn, ninja)
# Usage: ./scripts/build-crashpad.sh [--debug]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CRASHPAD_DIR="$PROJECT_ROOT/third_party/crashpad/crashpad"
BUILD_DIR="$CRASHPAD_DIR/out/Default"

# 1. Sync dependencies (mini_chromium, lss, zlib, googletest)
cd "$CRASHPAD_DIR/.."
gclient sync

# 2. Generate Ninja files
cd "$CRASHPAD_DIR"
gn gen "$BUILD_DIR" --args='
  is_debug=false
  target_cpu="x64"
'

# 3. Build only what we need
ninja -C "$BUILD_DIR" \
  crashpad_handler \
  client \
  client:common \
  util \
  third_party/mini_chromium
```

### 2. `third_party/crashpad/` — Git submodule

```bash
git submodule add https://chromium.googlesource.com/crashpad/crashpad \
    third_party/crashpad/crashpad
# Pin to a known stable commit
cd third_party/crashpad/crashpad && git checkout <stable-commit>
```

Also add a `.gclient` file in `third_party/crashpad/` so `gclient sync` knows
where to find the DEPS:

```python
solutions = [{
    "name": "crashpad",
    "url": "https://chromium.googlesource.com/crashpad/crashpad.git",
    "managed": False,
}]
```

### 3. `cmake/crashpad.cmake` — IMPORTED targets from prebuilt artifacts

A thin CMake module (~50-80 lines) that:
- Locates the prebuilt artifacts under `third_party/crashpad/crashpad/out/Default/`
- Creates IMPORTED STATIC library targets: `crashpad_client`, `crashpad_common`,
  `crashpad_util`, `crashpad_base` (mini_chromium)
- Creates an IMPORTED executable target: `crashpad_handler`
- Sets up include directories pointing at the Crashpad source tree
- Errors clearly if artifacts not found ("Run scripts/build-crashpad.sh first")

```cmake
# cmake/crashpad.cmake — IMPORTED targets for prebuilt Crashpad artifacts.
set(_crashpad_src "${CMAKE_SOURCE_DIR}/third_party/crashpad/crashpad")
set(_crashpad_build "${_crashpad_src}/out/Default")

if(NOT EXISTS "${_crashpad_build}/crashpad_handler")
  message(FATAL_ERROR
    "Crashpad not built. Run: ./scripts/build-crashpad.sh\n"
    "Expected artifacts in: ${_crashpad_build}")
endif()

# Static libraries
foreach(_lib client common util)
  add_library(crashpad_${_lib} IMPORTED STATIC GLOBAL)
  set_target_properties(crashpad_${_lib} PROPERTIES
    IMPORTED_LOCATION "${_crashpad_build}/obj/${_lib_subdir}/lib${_lib}.a"
  )
endforeach()

# mini_chromium base
add_library(crashpad_base IMPORTED STATIC GLOBAL)
set_target_properties(crashpad_base PROPERTIES
  IMPORTED_LOCATION "${_crashpad_build}/obj/third_party/mini_chromium/mini_chromium/base/libbase.a"
)

# Include directories (Crashpad headers + mini_chromium headers)
set(CRASHPAD_INCLUDE_DIRS
  "${_crashpad_src}"
  "${_crashpad_src}/third_party/mini_chromium/mini_chromium"
)

# Handler executable
add_executable(crashpad_handler IMPORTED GLOBAL)
set_target_properties(crashpad_handler PROPERTIES
  IMPORTED_LOCATION "${_crashpad_build}/crashpad_handler"
)

# Convenience: single target that links everything needed for client usage
add_library(crashpad_client_all INTERFACE)
target_link_libraries(crashpad_client_all INTERFACE
  crashpad_client crashpad_common crashpad_util crashpad_base
)
target_include_directories(crashpad_client_all SYSTEM INTERFACE
  ${CRASHPAD_INCLUDE_DIRS}
)
```

Exact library paths and names will be confirmed during implementation by inspecting
the actual GN build output.

---

## Modified files

### 4. CMakeLists.txt (root)

**Remove lines 105-202** (curl stub + sentry-native FetchContent + C_EXTENSIONS hack +
zlibstatic dependencies for sentry). The curl stub is no longer needed — Crashpad is built
externally with GN, which handles its own curl/transport compilation.

**Add** after the zlib block:
```cmake
include(cmake/crashpad.cmake)
```

**Note on zlib**: Keep the zlib FetchContent (lines 72-103) — it's still needed by Breakpad.
Crashpad's GN build handles its own zlib independently.

**Update install rule** (line 312-314): Comment change only ("via Crashpad" instead of
"via sentry-native"). The `crashpad_handler` IMPORTED target still works with `install()`.

### 5. lib/CMakeLists.txt — Change link target

```diff
- sentry
+ crashpad_client_all
```

Update header comment: "wrapper around Crashpad" instead of "wrapper around sentry-native".

### 6. lib/crashomon.cpp — Rewrite init/shutdown

Replace 11 sentry API calls with Crashpad's `CrashpadClient::StartHandler()`.

**Before** (sentry):
- `sentry_options_new()` + 5 option setters + `sentry_init(options)` + dummy DSN

**After** (Crashpad):
```cpp
#include "client/crashpad_client.h"
#include "client/crash_report_database.h"

namespace crashomon {
namespace {

crashpad::CrashpadClient& GetClient() {
  static crashpad::CrashpadClient client;
  return client;
}

int DoInit(const ResolvedConfig& cfg) {
  base::FilePath handler(cfg.handler_path);
  base::FilePath database(cfg.db_path);

  std::map<std::string, std::string> annotations;
  std::vector<std::string> arguments;

  bool ok = GetClient().StartHandler(
      handler, database, database,
      /*url=*/"",           // empty = no uploads
      annotations, arguments,
      /*restartable=*/true,
      /*asynchronous_start=*/false);

  return ok ? 0 : -1;
}

}  // namespace
}  // namespace crashomon
```

**Shutdown**: `crashomon_shutdown()` becomes a no-op (Crashpad handler is independent).

**Tags/breadcrumbs**: Make into explicit no-ops with TODO comments for Crashpad
annotation support as a follow-up. These were already dead code.

### 7. lib/crashomon.h — Update docs

- Update header comment to reference Crashpad instead of sentry-native.
- Add notes to tag/breadcrumb/abort_message functions that they are currently no-ops.
- No signature changes.

### 8. Documentation updates

**CLAUDE.md**:
- "Uses sentry-native (Crashpad backend)" -> "Uses Crashpad directly"
- Remove `SENTRY_TRANSPORT=none` reference
- Update dependency list and build instructions
- Add `depot_tools` to prerequisites
- Add `scripts/build-crashpad.sh` step before `cmake`

**DESIGN.md**:
- Update the sentry-native comparison table with migration rationale
- Update architecture references

**README.md**:
- Add Crashpad build prerequisite step
- Update build instructions: `./scripts/build-crashpad.sh && cmake -B build && cmake --build build`
- Document depot_tools requirement

---

## What does NOT change

- **lib/crashomon_internal.h** — Config resolution is backend-agnostic
- **daemon/** — Uses Breakpad's MinidumpProcessor only
- **tools/** — Uses minidump_stackwalk and eu-addr2line only
- **web/** — Uses minidump_stackwalk only
- **test/test_crashomon.cpp** — Tests Resolve()/GetEnv() only (no sentry linkage)
- **test/integration_test.sh** — End-to-end pipeline, backend-agnostic
- **cmake/breakpad.cmake** — Breakpad is a separate dependency

---

## Build workflow (after changes)

```bash
# One-time: fetch + build Crashpad (requires depot_tools in PATH)
./scripts/build-crashpad.sh

# Normal build (unchanged except Crashpad is prebuilt)
cmake -B build && cmake --build build && ctest --test-dir build
```

---

## Risks and mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| depot_tools is a heavy prerequisite | Medium | Document clearly; it's standard for Chromium ecosystem projects |
| GN output paths change across versions | Low | Pin submodule to specific commit; verify paths in script |
| Prebuilt .a files incompatible with project compiler flags | Low | Script uses same compiler; document required toolchain |
| IMPORTED crashpad_handler doesn't work with install() | Low | Use install(PROGRAMS) with the prebuilt binary path instead |
| Users unfamiliar with gclient/GN workflow | Medium | Script automates everything; README has copy-paste commands |

---

## Verification

1. **Crashpad build**: `./scripts/build-crashpad.sh` completes, artifacts exist
2. **CMake configure**: `cmake -B build` finds all IMPORTED targets
3. **Build**: `cmake --build build` — no warnings in project code
4. **Unit tests**: `ctest --test-dir build` — all existing tests pass
5. **Integration test**: `test/integration_test.sh build/`
6. **Manual LD_PRELOAD test**:
   ```bash
   LD_PRELOAD=build/lib/libcrashomon.so \
   CRASHOMON_DB_PATH=/tmp/test \
   CRASHOMON_HANDLER_PATH=third_party/crashpad/crashpad/out/Default/crashpad_handler \
   build/examples/crashomon-example-segfault
   # Verify .dmp file appears in /tmp/test/
   ```
7. **Sanitizers**: `-DENABLE_ASAN=ON`, `-DENABLE_UBSAN=ON`
8. **clang-tidy**: `-DENABLE_CLANG_TIDY=ON`

---

## What gets removed

- **Curl stub** (CMakeLists.txt lines 105-171) — GN handles Crashpad's curl dependency
- **sentry-native FetchContent** (lines 173-185) — replaced by submodule + prebuilt
- **C_EXTENSIONS workaround** (lines 187-193) — sentry-specific hack
- **sentry zlibstatic deps** (lines 195-202) — Crashpad builds its own zlib via GN
- **Dummy DSN** in crashomon.cpp — Crashpad has no DSN concept
- **All `sentry_*` API calls** in crashomon.cpp — replaced by CrashpadClient

---

## Follow-up (not in this PR)

- Implement `crashomon_set_tag()` with `crashpad::Annotation` — unlike sentry tags,
  Crashpad annotations are stored IN the minidump, readable by the daemon
- Implement `crashomon_set_abort_message()` with Crashpad annotations
- Reconsider breadcrumbs API (Crashpad has no breadcrumb concept)
