# Design: Crashomon Tombstone Redesign — Register-Only Crash Report

**Date:** 2026-04-17  
**Status:** Approved — ready for implementation  
**Scope:** `tombstone/minidump_reader.cpp`, `tombstone/minidump_reader.h`,
`tombstone/tombstone_formatter.cpp`

---

## 1. Problem

The current on-device tombstone uses Breakpad's `MinidumpProcessor` with no symbol
supplier, which falls back to heuristic stack scanning. This produces:

- Unreliable frame lists — bogus frames, mis-counted depths, false positives
- Output that differs from the host `crashomon-analyze` output (different algorithm,
  different fallback behavior) — confusing when diagnosing the same crash from both
- Continued dependency on the `MinidumpProcessor` / stackwalker for a use case that does
  not benefit from it (no symbols available on the embedded device)

The on-device tombstone's job is to give operators an immediate, 100%-reliable signal in
journald. Precise stack reconstruction belongs to `crashomon-analyze` on the host.

---

## 2. Goals and non-goals

**Goals:**
- Every field in the tombstone output is derived from data guaranteed to be in every
  Crashpad-generated minidump — no heuristics, no guesses
- Eliminate the stackwalker from the on-device path entirely
- Add a human-readable probable-cause line for high-confidence signal interpretations

**Non-goals:**
- Symbol resolution of any kind (no `.dynsym`, no symbol store, no on-disk ELF reads)
- Per-thread snapshots for non-crashing threads
- Generating a copy-pasteable `crashomon-analyze` command
- Printing CPU registers (not actionable in journald; full state is in the minidump)

---

## 3. Design decisions

| Question | Decision | Rationale |
|---|---|---|
| How many backtrace frames? | PC only — 1 frame from register context | 100% guaranteed from exception stream; no walking |
| Thread scope | Crashing thread only | Non-crashing threads add noise without reliable frames |
| Probable-cause line | Yes, conservative | High-confidence interpretations are immediately actionable; omit when ambiguous |
| Analyze command in output | No | Keeps output lean; minidump path is sufficient |
| Symbol resolution | No | `.dynsym` coverage for frame 0 in app binaries is low; not worth the complexity |
| Register block | No | Not actionable in journald; full state is in the minidump |
| Process/thread header | `process: <name> (pid <N>)  thread: <name> (tid <N>)` | Explicit labels are clearer than the Android `name: ... >>> ... <<<` convention |

---

## 4. Data sources

All data is read directly from the minidump via the low-level
`google_breakpad::Minidump` API. `MinidumpProcessor` is not used.

| Field | Minidump stream | API |
|---|---|---|
| Timestamp | `MDRawHeader::time_date_stamp` | `raw.header()->time_date_stamp` |
| PID | `MDRawMiscInfo::process_id` | `raw.GetMiscInfo()` |
| Signal number | `MDException::exception_code` | `raw.GetException()` |
| Signal code | `MDException::exception_flags` | `raw.GetException()` |
| Fault address | `MDException::exception_record.exception_address` | `raw.GetException()` |
| Crashing TID | `MDException::thread_id` | `raw.GetException()` |
| PC register | Thread context (ARM64 / AMD64) | `raw.GetThreadList()` + `GetThreadByID()` |
| SP register | Thread context (ARM64 / AMD64) | Same — used only for stack-overflow probable-cause check, not printed |
| Module list (path, base, size, build ID) | Module list stream | `raw.GetModuleList()` |
| Thread name | Thread names stream | `raw.GetThreadNameList()` |
| Process name | Basename of module at index 0 (lowest base address) | Module list |

The single crashing-thread frame is constructed as:

```
frame.pc            = PC register value
frame.module_path   = path of module where base_address <= pc < base_address + size
frame.module_offset = pc - module.base_address
frame.build_id      = module.build_id
```

If no module covers the PC, `module_path` is empty (rendered as `???`).

---

## 5. Probable-cause logic

Emit the probable-cause line only when the mapping is unambiguous. Omit entirely if
no rule matches (do not emit an uncertain guess).

The SP value used for the "near stack" check is taken from the crashing thread's
register context.

| Condition | Probable-cause text |
|---|---|
| `SIGSEGV` AND `fault_addr < 0x1000` | `null pointer dereference — access at offset 0x<fault_addr>` (offset omitted when fault_addr == 0) |
| `SIGSEGV` AND `fault_addr == pc` | `execute fault — non-executable memory at pc (corrupt function pointer, smashed stack, or ROP)` |
| `SIGSEGV` AND `|fault_addr - sp| < 65536` | `stack overflow — fault address 0x<fault_addr> is near stack pointer` |
| `SIGSEGV` (fallback, none of the above) | *(omit — too ambiguous to assert)* |
| `SIGABRT` | `abort() or assert() failure, or glibc heap corruption detected` |
| `SIGBUS` | `misaligned memory access or truncated mmap file` |
| `SIGILL` | `illegal instruction — corrupt code page or bad function pointer` |
| `SIGFPE` | `arithmetic exception — integer division by zero or overflow` |

Signal numbers use their standard Linux values: SIGSEGV=11, SIGABRT=6, SIGBUS=7,
SIGILL=4, SIGFPE=8.

---

## 6. Output format

```
*** *** *** *** *** CRASH DETECTED *** *** *** *** *** ***
process: <process_name> (pid <pid>)  thread: <thread_name> (tid <tid>)
signal <signum> (<SIGNAME>), code <code> (<CODENAME>), fault addr <fault_addr>
probable cause: <cause_text>
timestamp: <iso8601_utc>
pc <pc_offset>  <module_path> (BuildId: <build_id>)

minidump saved to: <minidump_path>
*** *** *** *** *** END OF CRASH *** *** *** *** *** ***
```

**Format rules:**
- `probable cause:` line is omitted when no rule matches
- `code <N> (<NAME>)` on the signal line is omitted when `signal_code == 0` or the
  code name is unknown
- `(BuildId: <build_id>)` is omitted when the build ID is empty
- If PC is not covered by any module: `pc <pc_absolute>  ???` (absolute address, no
  module offset, no build ID)
- `thread: <thread_name>` falls back to the process name when the thread has no name
- `pc <pc_offset>` is the offset within the module (not the absolute address)

---

## 7. Struct changes

`MinidumpInfo` (`minidump_reader.h`) changes:

- `threads` vector is retained but will contain **at most one entry** (the crashing
  thread). Non-crashing threads are no longer collected.
- `ThreadInfo::frames` will contain **at most one entry** (the PC frame from registers).
  The `frames` field is kept as `std::vector<FrameInfo>` to avoid changing the
  formatter's `AppendFrames` call site.
- No new fields added to any struct.

`FrameInfo` gains one field:

```cpp
std::string build_id;  // copied from the module's build_id at extraction time
```

This avoids the formatter needing to do a separate module-list lookup for the single
crashing frame.

---

## 8. Code changes summary

### `tombstone/minidump_reader.cpp`

- Remove `#include` of `minidump_processor.h`, `process_state.h`, `call_stack.h`,
  `stack_frame.h`, `process_result.h`
- Remove `CollectThreads`, `BuildThread`, `CollectModules` functions
- Remove `MinidumpProcessor` + `ProcessState` usage from `ReadMinidump`
- Add `ExtractExceptionInfo(raw, info)` — populates signal_number, signal_code,
  fault_addr, crashing_tid, signal_info string from `raw.GetException()`
- Add `ExtractModules(raw, info)` — populates `info.modules` from `raw.GetModuleList()`
- Add `ExtractProcessName(info)` — basename of lowest-base-address module
- Add `ExtractThreadName(raw, info)` — populates crashing thread's name from
  `raw.GetThreadNameList()`
- Add `BuildCrashingFrame(raw, info)` — creates single `FrameInfo` from PC register +
  module lookup; pushes one `ThreadInfo` into `info.threads`
- `ReadMinidump` becomes a linear sequence of these helpers; all via `raw`

### `tombstone/minidump_reader.h`

- Add `build_id` field to `FrameInfo`

### `tombstone/tombstone_formatter.cpp`

- Replace `pid: ..., tid: ..., name: ...  >>> ... <<<` header with
  `process: <name> (pid <N>)  thread: <name> (tid <N>)`; thread name falls back to
  process name when empty
- Replace top separator with `*** *** *** *** *** CRASH DETECTED *** *** *** *** *** ***`
- Replace bottom separator with `*** *** *** *** *** END OF CRASH *** *** *** *** *** ***`
- Remove register block entirely
- Remove `backtrace:` section; replace with bare `pc <offset>  <module> (BuildId: ...)` line
- Remove per-thread loop (only crashing thread now)
- Simplify `AppendFrames` to use `frame.build_id` directly; remove `build_ids` map
- Add `FormatProbableCause(signal_number, fault_addr, pc, sp) -> std::string` — returns
  empty string when no rule matches; emitted between signal line and timestamp

---

## 9. Files NOT changed

- `tombstone/tombstone_formatter.h` — unchanged (same public signature)
- Daemon's inotify/watcherd plumbing — unchanged
- `crashomon-analyze` — unchanged (uses its own separate path)
- Any tests for the formatter — need updating for new output format (separate task)

`tombstone/register_extract.h` / `.cpp` are no longer called from the formatter.
They remain in the tree (still used by `ReadMinidump` to extract PC and SP from the
thread context) but their output is not printed.
