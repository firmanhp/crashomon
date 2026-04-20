# Plan: Crashomon Tombstone Redesign — Implementation

**Spec:** `docs/superpowers/specs/2026-04-17-crashomon-tombstone-redesign-design.md`  
**Date:** 2026-04-17  

---

## Step 1 — Add `build_id` to `FrameInfo` in `minidump_reader.h`

File: `tombstone/minidump_reader.h`

Add one field to `FrameInfo`:

```cpp
struct FrameInfo {
  uint64_t pc = 0;
  uint64_t module_offset = 0;
  std::string module_path;
  std::string build_id;  // ADD: copied from containing module at extraction time
};
```

---

## Step 2 — Rewrite `ReadMinidump` in `minidump_reader.cpp`

File: `tombstone/minidump_reader.cpp`

### 2a — Remove dead includes

Remove these includes (no longer used after `MinidumpProcessor` is gone):

```cpp
#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/code_modules.h"
#include "google_breakpad/processor/minidump_processor.h"
#include "google_breakpad/processor/process_result.h"
#include "google_breakpad/processor/process_state.h"
#include "google_breakpad/processor/stack_frame.h"
```

Retain:

```cpp
#include "google_breakpad/common/minidump_format.h"
#include "google_breakpad/processor/minidump.h"
```

### 2b — Remove dead functions

Delete: `CollectModules`, `CollectThreads`, `BuildThread`.

Keep: `FormatTimestamp`, `Basename`, `ExtractRegisters`.

### 2c — Add `ExtractExceptionInfo`

```cpp
void ExtractExceptionInfo(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  auto* exc = raw.GetException();
  if (exc == nullptr) return;
  const auto* rec = exc->exception();
  if (rec == nullptr) return;

  info.signal_number = rec->exception_record.exception_code;
  info.signal_code   = rec->exception_record.exception_flags;
  info.fault_addr    = rec->exception_record.exception_address;
  info.crashing_tid  = rec->thread_id;

  // Build a signal_info string in the same "SIGNAME / CODENAME" format that
  // MinidumpProcessor previously provided.  Use a static lookup table for the
  // signal names that appear in the probable-cause logic; fall back to the
  // numeric value for anything else.
  static const std::unordered_map<uint32_t, const char*> kSigNames = {
      {4,  "SIGILL"},
      {6,  "SIGABRT"},
      {7,  "SIGBUS"},
      {8,  "SIGFPE"},
      {11, "SIGSEGV"},
  };
  const auto sig_it = kSigNames.find(info.signal_number);
  const std::string sig_name = (sig_it != kSigNames.end())
      ? sig_it->second
      : absl::StrCat("signal ", info.signal_number);

  // signal_code → Linux si_code names for SIGSEGV (most common).
  // Only the codes that actually appear in Crashpad minidumps on Linux.
  static const std::unordered_map<uint32_t, const char*> kSegvCodes = {
      {1, "SEGV_MAPERR"},
      {2, "SEGV_ACCERR"},
  };
  if (info.signal_number == 11 && info.signal_code != 0) {
    const auto code_it = kSegvCodes.find(info.signal_code);
    if (code_it != kSegvCodes.end()) {
      info.signal_info = absl::StrCat(sig_name, " / ", code_it->second);
      return;
    }
  }
  info.signal_info = sig_name;
}
```

### 2d — Add `ExtractModules`

```cpp
void ExtractModules(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  auto* module_list = raw.GetModuleList();
  if (module_list == nullptr) return;
  const unsigned int count = module_list->module_count();
  info.modules.reserve(count);
  for (unsigned int i = 0; i < count; ++i) {
    const auto* mod = module_list->GetModuleAtIndex(i);
    if (mod == nullptr) continue;
    info.modules.push_back({
        .path          = mod->code_file(),
        .base_address  = mod->base_address(),
        .size          = mod->size(),
        .build_id      = mod->code_identifier(),
    });
  }
}
```

### 2e — Add `BuildCrashingFrame`

Called after `ExtractRegisters` has populated `info.threads[0].registers`.

```cpp
// Looks up the module that contains |pc| and returns a FrameInfo.
// Returns an empty-module FrameInfo if pc is not covered by any module.
FrameInfo MakeFrameFromPC(uint64_t pc, const std::vector<ModuleInfo>& modules) {
  FrameInfo frame;
  frame.pc = pc;
  for (const auto& mod : modules) {
    if (pc >= mod.base_address && pc < mod.base_address + mod.size) {
      frame.module_path   = mod.path;
      frame.module_offset = pc - mod.base_address;
      frame.build_id      = mod.build_id;
      return frame;
    }
  }
  return frame;  // module_path empty → formatter prints ???
}
```

### 2f — Add `ExtractThreadName`

```cpp
void ExtractThreadName(google_breakpad::Minidump& raw, MinidumpInfo& info) {
  if (info.threads.empty()) return;
  auto* name_list = raw.GetThreadNameList();
  if (name_list == nullptr) return;
  const unsigned int count = name_list->thread_name_count();
  for (unsigned int i = 0; i < count; ++i) {
    const auto* entry = name_list->GetThreadNameAtIndex(i);
    if (entry == nullptr) continue;
    const auto* raw_name = entry->thread_name();
    if (raw_name == nullptr) continue;
    if (entry->thread_id() == info.crashing_tid) {
      info.threads[0].name = *raw_name;
      return;
    }
  }
}
```

### 2g — Rewrite `ReadMinidump`

```cpp
absl::StatusOr<MinidumpInfo> ReadMinidump(const std::string& path) {
  google_breakpad::Minidump raw(path);
  if (!raw.Read()) {
    return absl::InternalError(absl::StrCat("Failed to read minidump: ", path));
  }

  MinidumpInfo info;
  info.minidump_path = path;

  // Timestamp from header.
  if (const auto* hdr = raw.header()) {
    info.timestamp = FormatTimestamp(hdr->time_date_stamp);
  }

  // PID from misc info.
  if (auto* misc = raw.GetMiscInfo(); misc != nullptr && misc->misc_info() != nullptr) {
    if ((misc->misc_info()->flags1 & MD_MISCINFO_FLAGS1_PROCESS_ID) != 0U) {
      info.pid = misc->misc_info()->process_id;
    }
  }

  ExtractExceptionInfo(raw, info);
  ExtractModules(raw, info);

  if (!info.modules.empty()) {
    info.process_name = Basename(info.modules[0].path);
  }

  // Crashing thread: registers first, then derive frame 0 from PC register.
  if (info.crashing_tid != 0) {
    ThreadInfo thread;
    thread.tid         = info.crashing_tid;
    thread.is_crashing = true;
    info.threads.push_back(std::move(thread));
    ExtractRegisters(raw, info);

    // PC is the last entry of the registers vector (ARM64: index 32, AMD64: rip).
    // Derive frame 0 from it if registers were populated.
    if (!info.threads[0].registers.empty()) {
      // registers is in display order; find "pc" / "rip" by name.
      uint64_t pc = 0;
      for (const auto& [name, val] : info.threads[0].registers) {
        if (name == "pc" || name == "rip") {
          pc = val;
          break;
        }
      }
      if (pc != 0) {
        info.threads[0].frames.push_back(MakeFrameFromPC(pc, info.modules));
      }
    }

    ExtractThreadName(raw, info);
  }

  return info;
}
```

---

## Step 3 — Update `tombstone_formatter.cpp`

File: `tombstone/tombstone_formatter.cpp`

### 3a — Add `FormatProbableCause`

Add this free function in the anonymous namespace:

```cpp
// Returns a non-empty string when the signal+address combination maps
// unambiguously to a known cause; empty string otherwise.
std::string FormatProbableCause(uint32_t signal_number, uint64_t fault_addr,
                                uint64_t pc, uint64_t sp) {
  constexpr uint32_t kSIGSEGV = 11;
  constexpr uint32_t kSIGABRT = 6;
  constexpr uint32_t kSIGBUS  = 7;
  constexpr uint32_t kSIGILL  = 4;
  constexpr uint32_t kSIGFPE  = 8;
  constexpr uint64_t kNullPageLimit  = 0x1000;
  constexpr uint64_t kStackProximity = 65536;

  if (signal_number == kSIGSEGV) {
    if (fault_addr < kNullPageLimit) {
      if (fault_addr == 0) {
        return "null pointer dereference";
      }
      return absl::StrCat("null pointer dereference — access at offset 0x",
                          absl::Hex(fault_addr));
    }
    if (fault_addr == pc) {
      return "execute fault — non-executable memory at pc "
             "(corrupt function pointer, smashed stack, or ROP)";
    }
    if (sp != 0) {
      const uint64_t delta = (fault_addr > sp) ? (fault_addr - sp) : (sp - fault_addr);
      if (delta < kStackProximity) {
        return absl::StrCat(
            "stack overflow — fault address 0x", absl::Hex(fault_addr),
            " is near stack pointer");
      }
    }
    return "";  // ambiguous — omit
  }
  if (signal_number == kSIGABRT) {
    return "abort() or assert() failure, or glibc heap corruption detected";
  }
  if (signal_number == kSIGBUS) {
    return "misaligned memory access or truncated mmap file";
  }
  if (signal_number == kSIGILL) {
    return "illegal instruction — corrupt code page or bad function pointer";
  }
  if (signal_number == kSIGFPE) {
    return "arithmetic exception — integer division by zero or overflow";
  }
  return "";
}
```

Note: `FormatProbableCause` needs `absl/strings/str_cat.h` and `absl/strings/numbers.h`
(for `absl::Hex`) — `str_cat.h` is already included. Add `absl/strings/numbers.h` if
needed, or use `std::format` for the hex formatting to avoid a new include.

### 3b — Extract SP from crashing thread registers

Add a small helper in the anonymous namespace:

```cpp
uint64_t ExtractSP(const std::vector<std::pair<std::string, uint64_t>>& regs) {
  for (const auto& [name, val] : regs) {
    if (name == "sp" || name == "rsp") return val;
  }
  return 0;
}
```

### 3c — Update `FormatTombstone`

- Remove `build_ids` map construction (no longer needed — `FrameInfo` now carries
  `build_id` directly)
- Add probable-cause line between signal line and timestamp
- Rename section header `backtrace:` → `crashing instruction:`
- Remove the non-crashing threads loop (the `threads` vector has at most one entry now,
  but the existing `if (!info.threads.empty() && info.threads[0].is_crashing)` guard
  already handles this correctly — just change the header text)

Update `AppendFrames` to use `frame.build_id` directly:

```cpp
void AppendFrames(std::ostringstream& out, const std::vector<FrameInfo>& frames) {
  for (size_t idx = 0; idx < frames.size(); ++idx) {
    const auto& frame = frames[idx];
    if (frame.module_path.empty()) {
      out << std::format("    #{} pc 0x{:016x}  ???\n", idx, frame.pc);
    } else if (!frame.build_id.empty()) {
      out << std::format("    #{} pc 0x{:016x}  {} (BuildId: {})\n",
                         idx, frame.module_offset, frame.module_path, frame.build_id);
    } else {
      out << std::format("    #{} pc 0x{:016x}  {}\n",
                         idx, frame.module_offset, frame.module_path);
    }
  }
}
```

In `FormatTombstone`, the backtrace section becomes:

```cpp
// Probable cause.
if (!info.threads.empty() && info.threads[0].is_crashing) {
  const uint64_t sp = ExtractSP(info.threads[0].registers);
  const uint64_t pc = info.threads[0].frames.empty() ? 0
                                                      : info.threads[0].frames[0].pc;
  const std::string cause =
      FormatProbableCause(info.signal_number, info.fault_addr, pc, sp);
  if (!cause.empty()) {
    out << "probable cause: " << cause << "\n";
  }
}

out << "timestamp: " << info.timestamp << "\n";

// ... (register block unchanged) ...

// Crashing instruction.
if (!info.threads.empty() && info.threads[0].is_crashing) {
  out << "\ncrashing instruction:\n";
  AppendFrames(out, info.threads[0].frames);
}
```

---

## Step 4 — Update existing tests

File: `test/` — find tombstone formatter/reader tests and update expected output:

- Replace `backtrace:` with `crashing instruction:` in expected strings
- Add `probable cause:` line to expected output where applicable
- Remove expected non-crashing thread sections

---

## Step 5 — Build and verify

```bash
cmake -B build && cmake --build build && ctest --test-dir build
```

Then run the end-to-end demo:

```bash
cd demo && ./run_demo.sh
```

Confirm tombstone in journald (or stdout) shows the new format: no multi-frame backtrace,
`crashing instruction:` section with a single PC line, `probable cause:` line.

---

## Checklist

- [ ] Step 1: `build_id` field in `FrameInfo`
- [ ] Step 2: rewrite `minidump_reader.cpp`
- [ ] Step 3: update `tombstone_formatter.cpp`
- [ ] Step 4: update tests
- [ ] Step 5: build + ctest green + demo verified
