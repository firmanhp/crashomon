# Analysis Plugins for crashomon-analyze

## Goal

Add a modular analysis layer to `crashomon-analyze` that reads a tombstone or
minidump and emits structured, actionable findings — without polluting the
existing analysis modes.  Each plugin is a self-contained class with a single
`analyze()` staticmethod.  The user (or the tool) picks which plugins to run.

---

## Plugin interface

```python
# tools/analyze/plugins/__init__.py

from __future__ import annotations
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Finding:
    plugin: str        # class name, e.g. "StackOverflowLikely"
    confidence: str    # "certain" | "likely" | "possible"
    headline: str      # one-line human message (fits in a terminal line)
    detail: str = ""   # optional multi-line explanation / next steps


def run_all(tombstone, plugins=None) -> list[Finding]:
    """Run every registered plugin (or a subset) against a ParsedTombstone.

    Returns findings in plugin-registration order, skipping None returns.
    """
    ...
```

Two plugin shapes depending on the data source:

```python
# Tombstone-only plugin (no extra tools needed)
class StackOverflowLikely:
    @staticmethod
    def analyze(tombstone: ParsedTombstone) -> Finding | None: ...

# Minidump binary plugin (needs raw .dmp bytes)
class AddressRegionClassifier:
    @staticmethod
    def analyze(tombstone: ParsedTombstone, dmp_data: bytes) -> Finding | None: ...
```

Plugins live under `tools/analyze/plugins/`.  Each plugin is its own file.
`plugins/__init__.py` imports them all and exposes `TOMBSTONE_PLUGINS` and
`MINIDUMP_PLUGINS` lists.  Neither list pollutes `analyze.py` or `symbolizer.py`.

---

## Data model extensions required

### 1. Registers in ParsedTombstone

The C++ daemon emits registers in the tombstone text but `log_parser.py` does
not parse them.  Add to `ParsedTombstone`:

```python
registers: dict[str, int] = field(default_factory=dict)
# e.g. {"rax": 0, "rbx": 0x7f..., "rip": 0x401234, "rsp": 0x7fff...}
```

And extend `parse_tombstone()` to recognise the register block:

```
   rax 0000000000000000 rbx 00007f1234567890 rcx 0000000000000001
```

Pattern: lines between the signal line and `backtrace:` that match
`^\s{3}(\w+)\s+([0-9a-f]{16})` repeated up to 3 times per line.
Register names are architecture-dependent; store whatever names the daemon
emits — no hardcoded list needed.

### 2. Memory maps from minidump

For plugins that classify the fault address by region (stack, heap, code,
unmapped), the minidump binary carries a `MemoryInfoListStream` or a
`MemoryListStream`.  Add a helper in `symbolizer.py`:

```python
def read_minidump_memory_regions(dmp_path: str) -> list[MemoryRegion]: ...

@dataclass
class MemoryRegion:
    base: int
    size: int
    state: str   # "commit" | "reserve" | "free"
    protect: str # "rwx" bitmask string, e.g. "r-x"
    region_type: str  # "private" | "mapped" | "image"
```

This is used only by the minidump-shape plugins; tombstone-only plugins have
no dependency on it.

---

## Plugins — ordered by false alarm probability (lowest first)

Each entry: name, data source, what it detects, why false alarms are rare.

---

### 1. `CrashClassifier`  — tombstone, certain

Maps `(signal_name, signal_code_name)` deterministically to a human label.
No heuristic — the OS chose the signal and code.

| signal | code | label |
|--------|------|-------|
| SIGSEGV | SEGV_MAPERR | bad pointer / null deref |
| SIGSEGV | SEGV_ACCERR | permission fault (write to RO? exec stack?) |
| SIGBUS | BUS_ADRALN | unaligned access |
| SIGBUS | BUS_ADRERR | non-existent physical address |
| SIGILL | ILL_ILLOPC | illegal opcode |
| SIGILL | ILL_ILLOPN | illegal operand |
| SIGFPE | FPE_INTDIV | integer divide by zero |
| SIGFPE | FPE_INTOVF | integer overflow (trap) |
| SIGABRT | — | explicit abort / allocator / assertion |

Output example:
```
[certain] PERMISSION_FAULT — SIGSEGV/SEGV_ACCERR: write to read-only mapping
          Possible causes: stack overflow touching guard page, write through
          const pointer, JIT emitting into RX page.
```

False alarm rate: 0%.  The kernel sets these values; they are not
approximations.

---

### 2. `SigabrtAbort`  — tombstone, certain

Fires only on `SIGABRT`.  SIGABRT sent to oneself (which is what `abort()`,
`__assert_fail`, `__stack_chk_fail`, and the allocator double-free detector
all do) means the program terminated itself deliberately.  The fault address is
meaningless for SIGABRT — suppress it and redirect attention to the backtrace.

Output:
```
[certain] EXPLICIT_ABORT — process called abort() or triggered an assertion/
          allocator check. Fault address is not meaningful. Focus on the
          backtrace: look for __assert_fail, __stack_chk_fail, or allocator
          frames.
```

False alarm rate: 0%.  SIGABRT from the process to itself has one cause.

---

### 3. `MissingSymbols`  — tombstone, certain

Scans all crashing-thread frames.  Any frame where `module_path` is non-empty
but `trailing` (symbol text) is empty is unsymbolicated.  Collect the distinct
modules and their build IDs.

Output:
```
[certain] MISSING_SYMBOLS — 2 module(s) not symbolicated:
          libfoo.so  BuildId: a1b2c3d4...
          libbar.so  BuildId: e5f6a7b8...
          Run: crashomon-syms add <debug-binary> for each module above.
```

False alarm rate: 0%.  A missing symbol is not a heuristic — the frame either
resolved or it did not.  Only fires when symbolication was attempted (mode 1/3)
or when BuildId is present (mode 2).

---

### 4. `DebuggerHint`  — tombstone, certain

Pure formatting — always correct.  Emits a ready-to-paste `gdb` and `lldb`
command from the minidump path in the tombstone.  No analysis, just removes
the "how do I open this again?" friction.

Output:
```
[certain] DEBUGGER_HINT
          gdb -c /var/lib/crashomon/dumps/abc123.dmp /path/to/binary
            (gdb) info registers
            (gdb) bt full
          lldb --core /var/lib/crashomon/dumps/abc123.dmp /path/to/binary
            (lldb) bt all
            (lldb) register read
```

Only fires when `tombstone.minidump_path` is non-empty.  False alarm rate: 0%.

---

### 5. `CorruptPc`  — tombstone, ~0%

Fires when frame #0 has no module (`module_path == ""`) AND the pc value
stored in `module_offset` is in a suspicious range: 0, 1, 2, or
`module_offset % 4 != 0` on architectures where instructions are 4-byte
aligned (ARM64).

A corrupt PC means the backtrace that follows is garbage.  The engineer should
not trust the frames and should open the minidump in a debugger instead.

Output:
```
[certain] CORRUPT_PC — frame #0 PC is 0x0000000000000001 (not a valid code
          address). The backtrace below is unreliable. Open the minidump in
          gdb/lldb to inspect register state directly.
```

False alarm rate: ~0%.  A PC of 0x1 or 0x3 is never a valid instruction
address on any supported architecture.

---

### 6. `NullFunctionPointer`  — tombstone, ~1%

Distinct from a null data deref.  A null function pointer call results in:
- `signal = SIGSEGV`, `code = SEGV_MAPERR`
- `fault_addr == 0` (the address that was fetched for execution)
- frame #0 module is `???` (PC became 0)

Contrast with a null *data* deref: frame #0 has a valid module, fault_addr
is near 0.

Output:
```
[likely] NULL_FUNCTION_POINTER — execution attempted at address 0x0. This is
         a call through an uninitialized or explicitly null function pointer.
         Look for the function pointer assignment site in the caller (frame #1).
```

False alarm rate: ~1%.  The only alternative is a deliberately crafted call to
address 0, which does not occur in normal code.

---

### 7. `StackOverflowLikely`  — tombstone + registers, ~2%

Requires `registers` to be parsed (see data model extension above).

Condition: `abs(tombstone.fault_addr - sp) < 128 * 1024` where `sp` is
the stack pointer register (`rsp` on x86-64, `sp` on ARM64).

The 128 KB threshold covers the default Linux stack guard size (4 KB) plus
typical deep-recursion overshoot.  The backtrace is often corrupt when the
stack overflows, making this the only surviving diagnostic.

Also check: `signal == SIGSEGV`, `code == SEGV_MAPERR` (the guard page is
mapped but not committed, giving a MAPERR, not ACCERR).

Output:
```
[likely] STACK_OVERFLOW — fault addr 0x7fff12340000 is 4096 bytes below
         SP 0x7fff12341000. The stack guard page was hit. The backtrace may
         be incomplete or corrupt due to the blown stack.
         Default Linux thread stack: 8 MB. Check for deep recursion or
         large stack-allocated arrays.
```

False alarm rate: ~2%.  A non-overflow crash landing within 128 KB of the
stack pointer is uncommon but possible in adversarial or obfuscated code.

---

### 8. `IndirectCallCorruption`  — tombstone + registers, ~3%

Requires registers.  Fires when:
- Frame #0 has no valid module (PC is clearly bad / `???`)
- LR (`x30` on ARM64, or `lr`) holds a value that resolves to a known module

A valid LR with a corrupt PC is almost always a bad vtable pointer, a
use-after-free of an object with a vtable, or a corrupted function pointer
that was called via `blr`/`call`.

Output:
```
[likely] INDIRECT_CALL_CORRUPTION — PC is invalid (0xdeadbeef) but LR is
         valid: libfoo.so+0x1234. The crash occurred via a corrupt function
         pointer or vtable. Examine the object passed to the function
         identified by LR.
```

False alarm rate: ~3%.  Valid LR + invalid PC has few benign explanations.

---

### 9. `NullDerefOffset`  — tombstone, ~3%

Fires when:
- `signal == SIGSEGV`, `code == SEGV_MAPERR`
- `0 < fault_addr < 4096` (non-zero, but in the null page)
- Frame #0 has a valid module (so the PC is good — it's a data access, not a
  function call)

The offset within the null page is the struct field offset that was accessed.

Output:
```
[likely] NULL_DEREF_OFFSET — fault addr 0x18 (offset 24 / 0x18 from null).
         A field at offset 24 was accessed through a null pointer.
         Frame #0 is the access site. Check what pointer was null in that
         function.
```

False alarm rate: ~3%.  A fault at 0x18 in a SIGSEGV/MAPERR crash is almost
always a null+offset deref.  The rare alternative is a very small non-null
bad pointer.

---

### 10. `CallerFromLr`  — tombstone + registers, ~5%

Fallback for when the backtrace is corrupt or empty (e.g., after a stack
overflow or corrupt frame pointer chain).  If `registers["lr"]` (or `x30`) is
non-zero and not obviously corrupt, emit it as an approximate caller hint.

Only fires when the backtrace has fewer than 2 frames or frame #1 is `???`.

Output:
```
[possible] CALLER_FROM_LR — backtrace is short/corrupt. LR register suggests
           the direct caller was: libfoo.so+0x5678 (approximate — LR may
           have been overwritten).
```

False alarm rate: ~5–10%.  LR may have been clobbered before the crash, but
in the common case (crash in a leaf function or signal handler) it is valid.
Marked `possible` to signal the uncertainty.

---

### 11. `SuspiciousZeroArgs`  — tombstone + registers, ~10%

Fires when all of the following hold:
- `signal == SIGSEGV`, `code == SEGV_MAPERR`
- At least one of the argument registers (`rdi`, `rsi`, `rdx` on x86-64;
  `x0`–`x3` on ARM64) is `0`
- Frame #0 has a valid symbol (so we know what function was called)

Output:
```
[possible] SUSPICIOUS_ZERO_ARG — register x0 = 0x0 at crash site in
           my_func(). If x0 holds the first argument, a null was passed.
           Note: registers reflect state at signal delivery, not necessarily
           at the function entry — treat as a hint.
```

Marked `possible` because argument registers can be reused after function
entry and may not hold the original argument at the crash instruction.
False alarm rate: ~10–20%.  Useful as a hint, not a conclusion.

---

### 12. `AddressRegionClassifier`  — minidump, ~5%

Requires minidump binary (memory region list).  Classifies `fault_addr` by
region type from the `MemoryInfoListStream`:

| Region state | Protection | Inferred |
|---|---|---|
| committed | r-x (code) | fault in code segment (write-to-code? self-modify?) |
| committed | rw- (stack/heap) | fault in live data |
| committed | r-- (rodata) | write to read-only constant |
| free / not present | — | true unmapped access |

Works independently of the signal code — the memory map is ground truth.

Output:
```
[likely] ADDRESS_IN_CODE_SEGMENT — fault addr 0x401234 is in a committed
         executable region of libfoo.so. This is a write to a code page.
         Possible causes: self-modifying code, JIT without mprotect, or
         buffer overflow reaching the .text section.
```

False alarm rate: ~5%.  Memory map is authoritative; classification errors
come only from stale snapshot timing (region was remapped between crash and
minidump write — rare).

---

## Integration with crashomon-analyze

Add a `--analyze` flag to the CLI.  After formatting the symbolicated output,
run all registered plugins against the `ParsedTombstone` and append findings:

```
*** *** *** *** ... (tombstone) ... *** *** ***

Analysis findings:
[certain]  EXPLICIT_ABORT — process called abort() ...
[certain]  MISSING_SYMBOLS — libfoo.so (BuildId: a1b2...) not symbolicated
[certain]  DEBUGGER_HINT — gdb -c /var/lib/.../abc123.dmp ...
```

Findings are appended after the tombstone, not interleaved, so the tombstone
remains copy-pasteable.

The flag is opt-in: existing modes are unaffected.  Future: `--analyze=stack,pc`
to run a named subset.

---

## File layout

```
tools/analyze/
  plugins/
    __init__.py            # Finding, run_all(), TOMBSTONE_PLUGINS, MINIDUMP_PLUGINS
    crash_classifier.py    # CrashClassifier
    sigabrt_abort.py       # SigabrtAbort
    missing_symbols.py     # MissingSymbols
    debugger_hint.py       # DebuggerHint
    corrupt_pc.py          # CorruptPc
    null_function_ptr.py   # NullFunctionPointer
    stack_overflow.py      # StackOverflowLikely
    indirect_call.py       # IndirectCallCorruption
    null_deref_offset.py   # NullDerefOffset
    caller_from_lr.py      # CallerFromLr
    suspicious_zero_args.py # SuspiciousZeroArgs
    address_region.py      # AddressRegionClassifier
```

Each file contains exactly one class.  No plugin imports another plugin.
Dependencies flow only inward: plugins import from `log_parser` and
`symbolizer`, never from `analyze.py`.
