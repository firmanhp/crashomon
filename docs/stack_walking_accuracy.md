# Stack Walking Accuracy

Stack walking in crashomon happens in two distinct contexts with fundamentally different
capabilities. This document explains what each context can and cannot do, why they differ,
and what conditions break each one.

---

## The two contexts

**On-target** (`crashomon-watcherd`): the daemon processes the minidump immediately on the
device and prints an Android-style tombstone to journald. The device has no symbol files.
All binaries are stripped. The daemon has access only to what is in the minidump itself:
raw stack memory and register state.

**On-host** (`crashomon-analyze`): the same minidump is processed offline on a development
machine that has access to the symbol store. `minidump-stackwalk` is invoked with `.sym` files
that were extracted from the unstripped binaries before deployment. The on-host analysis is
the authoritative one.

The same minidump feeds both analyses. They diverge entirely in what they can do with it.

---

## On-target stack walking

### What is available

With no `.sym` files, Breakpad's minidump-processor cannot use Call Frame Information.
[Breakpad's documentation](https://chromium.googlesource.com/breakpad/breakpad/+/HEAD/docs/stack_walking.md)
describes the cascade it uses per frame:

1. CFI — **unavailable** (no `.sym` files on device)
2. Frame pointer — available if every frame in the chain has `-fno-omit-frame-pointer`
3. Stack scanning — heuristic last resort

The on-target tombstone is therefore produced entirely by frame pointer walking, falling back
to scanning where the chain breaks. It yields raw addresses with no function names, no file
names, and no line numbers.

### Frame pointer walking

On x86-64, a function compiled with `-fno-omit-frame-pointer` opens with:
```asm
push rbp
mov  rbp, rsp
```
The return address is always at `[rbp+8]`. The previous frame's base is at `[rbp]`. Walking
the stack is a singly-linked list traversal: follow the `rbp` chain until it reaches zero
or leaves the stack region.

**The chain-completeness requirement.**
Frame pointer walking requires that *every* frame in the call chain was compiled with
`-fno-omit-frame-pointer`. If any shared library — including libc, libstdc++, or any
third-party `.so` — was compiled without it, the chain snaps at that frame. Nothing below
it is reachable by this method.

**The top-of-stack case is the worst case.**
Frame 0 (the crashed function) is always known — Crashpad captured the register state
directly. Recovering frame 1 requires reading `rbp` from that captured context. If the crash
occurred in a library with no frame pointers, `rbp` in the crash context is an arbitrary
register value, not a frame pointer at all. The very first transition fails, and scanning
takes over immediately for the rest of the trace.

**Prologue/epilogue window.**
Between the moment `push rbp` executes and the moment `mov rbp, rsp` completes, `rbp` holds
the *caller's* frame pointer, not the current frame's. A crash (or signal) during this window
causes the walker to misidentify the current frame and attribute its callers one level up.
[Red Hat's 2024 analysis](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding)
quantified this: at least 5.2% of samples on x86-64 and 6.0% on aarch64 fall within the
prologue window. The actual rate is higher because "compiler optimizations may expand the
prologue with additional initialization code."

Shrink-wrapping enlarges this window further. When this optimization defers the prologue to
a non-entry basic block,
[the LLVM shrink-wrapping review](https://reviews.llvm.org/D9210) notes that "when shrink
wrapping is enabled, the prologue may not be at the beginning of the function... unwinding
cannot be performed correctly in this case."

**Assembly functions break the chain.**
Hand-written assembly implementations (many glibc functions: `memcpy`, `strcpy`, `memmove`)
do not establish frame pointers.
[Red Hat, 2024](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding)
documents two outcomes:
- *Best case:* "the frame pointer unwind will skip the caller of the assembly-code function.
  That is, if function `f` calls function `g` which calls `strcpy`, the resulting stack trace
  will claim that function `f` called `strcpy` directly."
- *Worst case:* "if the assembly-code function uses the frame pointer register for
  general-purpose computation, the unwind will not be able to proceed at all."

**PLT stubs break the chain.**
Every call to a dynamically linked function passes through a PLT stub. PLT stubs are
linker-synthesized and do not set up frame pointers. From
[Red Hat, 2024](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding):
"Profiles produced by frame pointer unwinding will inevitably exhibit gaps around function
prologues and epilogues and in procedure lookup table (PLT) sections."

**Conclusion.**
Even with `-fno-omit-frame-pointer` applied globally to every binary in the sysroot,
[Red Hat 2024](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding)
concludes: "call-frame information is still required to obtain an accurate profile." On-target
walking is inherently approximate.

### Stack scanning

When the frame pointer chain breaks, the walker falls back to scanning: treating each
word-aligned value on the stack as a candidate return address and checking whether it points
into a known executable region.

[Mozilla's post-mortem on rust-minidump](https://hacks.mozilla.org/2022/06/everything-is-broken-shipping-rust-minidump-at-mozilla/)
describes the failure mode directly: "the stackwalker found something that looked plausible
enough and went on a wacky adventure through the stack and made up a whole pile of useless
garbage frames." The stack contains stale return addresses from prior calls, saved registers,
function pointers, and other values that are valid code addresses but do not reflect the
current call chain. Near the bottom of the stack this problem is worst: the stack has the
most historical data from previous calls.

**Scan-recovered frames must be treated as unreliable hints, not facts.**

### What on-target walking gives you

At best: a sequence of addresses that correctly reflects the call chain, useful for immediate
triage and identification of the crashing module. At worst: a plausible-looking sequence of
addresses that is partially or entirely fabricated. The tombstone cannot tell you which frames
are reliable and which are not without the symbol information that establishes where functions
begin and end.

---

## On-host stack walking

### What is available

With `.sym` files present, `minidump-stackwalk` can use all three strategies in the cascade.
The `.sym` file contains:
- `STACK CFI` records — extracted from `.eh_frame` by `dump_syms`, covering each instruction
  in the binary with rules for recovering the caller's register state
- `FUNC` records — function name, start address, size
- `LINE` records — source file and line number for each address range
- `PUBLIC` records — exported symbol addresses

CFI is attempted first. It is both more accurate than frame pointers and covers cases frame
pointers cannot handle (prologue/epilogue windows, inline assembly, PLT stubs — to the extent
those have CFI coverage).

### CFI walking

The compiler emits CFI alongside every function describing, at every instruction, the offset
from the current stack pointer at which the return address is stored and how to recover the
caller's frame. On x86-64 Linux this is generated by default even at `-O3`, and even when C++
exception handling is disabled (`-fno-exceptions`): the System V AMD64 ABI
mandates asynchronous unwind tables so that debuggers, profilers, and signal
dispatch can unwind at any instruction. GCC implements this via
`-fasynchronous-unwind-tables`, which is on by default on x86-64 Linux
([GCC instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html)).
Unwind table generation is suppressed only by `-fno-asynchronous-unwind-tables`.

`dump_syms` extracts this from `.eh_frame` in the *unstripped* binary. The stripped binary
deployed to the device retains the same code and the same build ID; `minidump-stackwalk` uses
the build ID to look up the matching `.sym` file and applies its CFI to the addresses in the
minidump.

**When CFI is accurate.**
CFI is accurate for all standard compiler-generated code: C, C++, Rust, anything compiled
by GCC or Clang without disabling unwind tables.

**When CFI fails or has gaps.**

*No `.sym` file for a library.* If a shared library is present in the crash trace but has no
corresponding `.sym` file in the symbol store, CFI is unavailable for that library's frames.
The walker falls back to frame pointer for those frames, then scanning if frame pointers are
also absent.

*`-fno-asynchronous-unwind-tables`.* Explicitly suppresses `.eh_frame` generation.
Sometimes set in embedded builds to reduce binary size. `dump_syms` will produce a `.sym`
file with `FUNC` records (if the binary has debug symbols) but no `STACK CFI` records.

*Inline assembly modifying `rsp`.*
[MaskRay's stack unwinding reference](https://maskray.me/blog/2020-11-08-stack-unwinding)
notes: "GCC does not parse inline assembly, so adjusting SP in inline assembly often results
in imprecise CFI." The emitted CFI reflects the compiler's model of the stack, which diverges
from reality at the assembly block.

*Hand-written assembly without `.cfi_*` annotations.*
Assembly files get no automatic CFI. Many glibc functions are written in assembly without CFI
annotations. `dump_syms` extracts nothing for these functions.

*PLT stubs.*
[MaskRay](https://maskray.me/blog/2020-11-08-stack-unwinding): "Linker generated code (PLT
and range extension thunks) generally does not have unwind information coverage." GNU `ld`
supports `--ld-generated-unwind-info` to synthesize CFI for PLT entries, but this is not
universally enabled.

*Compiler bugs in CFI generation.*
The OOPSLA 2019 paper
["Reliable and Fast DWARF-Based Stack Unwinding"](https://kar.kent.ac.uk/76575/)
(Bastian, Kell, Zappa Nardelli) found that "generating debug information adds significant
burden to compiler implementations, and the debug information itself can be pervaded by subtle
bugs, making the whole infrastructure unreliable." This is uncommon for mainstream compilers
on common architectures but is not zero.

### CFI + inlining: logical vs. physical frames

With a `.sym` file, on-host analysis can reconstruct inlined calls that have no physical
stack frame. The DWARF `DW_TAG_inlined_subroutine` records embedded in the binary (and
reflected in the `.sym` file's `INLINE` records) let `minidump-stackwalk` expand a single
physical frame into the sequence of logical calls that were inlined into it.

On-target, inlined functions are invisible — they have no frame, so frame pointer walking
and scanning cannot see them. They appear merged into their physical caller. This is a
structural difference between the two analyses of the same crash.

### What on-host walking gives you

A symbolicated call chain: function names, source files, line numbers, and expanded inline
frames — for every frame whose library has a `.sym` file. Frames in libraries without `.sym`
files are shown as addresses only, with the walk quality for those frames falling back to the
same constraints as on-target (frame pointer or scan).

---

## Shared constraints: what affects both analyses equally

Certain conditions degrade both on-target and on-host analyses because they affect what the
minidump itself contains, upstream of any unwinding strategy.

### Minidump stack memory capture limits

`minidump-stackwalk` can only unwind as deep as the stack memory that was captured.
Crashpad captures stack memory starting from the stack pointer at crash time. Frames whose
stack data lies outside the captured region are inaccessible regardless of whether CFI or
frame pointers are available.

[Sentry's investigation of SteamOS crashes](https://blog.sentry.io/not-so-mini-dumps-how-we-found-missing-crashes-on-steamos/)
found that deriving capture boundaries from Thread Environment Block values is unreliable on
non-native platforms: Wine/Proton's TEB implementation set stack limit fields to the entire
user-mode address space, producing minidumps "several hundreds of megabytes" in size per
thread. The robust approach is to capture relative to the actual stack pointer at crash time.

### Signal frame (crash in a signal handler)

When a crash occurs inside a signal handler, the kernel pushes a `ucontext_t` onto the stack
before invoking the handler. This creates a special frame that does not follow normal calling
conventions.

**Frame pointer (on-target and on-host fallback):** cannot cross the signal frame boundary.
`rbp` inside the signal handler points into the handler's own frame, not through `ucontext_t`
to the interrupted thread's context. The pre-signal call chain is unreachable by frame pointer.

**CFI (on-host):** works if and only if the `sigreturn` trampoline has CFI annotations
describing the `ucontext_t` layout.
[MaskRay's signal handler unwinding article](https://maskray.me/blog/2022-04-10-unwinding-through-signal-handler)
documents the platform split:
- glibc x86-64: "provides detailed CFI describing `ucontext_t` register offsets" — crossing
  the signal frame works.
- **musl libc: "lacks CFI"** for its trampoline — the walk halts at the signal frame on
  both on-target and on-host.
- AArch64 (any libc): "AArch64's DWARF approach cannot simultaneously describe both the
  previous PC and previous X30 (link register), creating ambiguity in signal frame unwinding."

**Consequence:** On a musl-based embedded system, a crash inside a signal handler will produce
a tombstone and an offline analysis that both terminate at the signal frame. The pre-signal
context is lost in both analyses.

### Tail call elimination

When a function's last operation is a call, the compiler emits a jump rather than a call +
return, reusing the current frame. The intermediate function's frame does not exist at crash
time.
[Wikipedia on tail calls](https://en.wikipedia.org/wiki/Tail_call): "the tail-called function
will return directly to the original caller." Neither frame pointer walking, scanning, nor CFI
can recover a frame that was never pushed. Both analyses will omit tail-called functions.

### Corrupted stack at crash time

Some crashes (stack overflow, buffer overrun overwriting return addresses) corrupt the stack
before Crashpad captures it. All three methods — CFI, frame pointer, scan — operate on the
captured stack memory. If that memory is corrupted, all methods will produce garbage, or will
stop early when the CFI-recovered frame pointer leaves the valid stack region.

---

## The dependency chain: what each analysis needs

### On-target analysis requires

1. **Frame pointers in every library** in the call chain — including libc, libstdc++, and all
   third-party `.so` files. Enforced via global Yocto/Buildroot `CFLAGS`. One missing library
   breaks the chain.
2. **Sufficient stack memory capture** in the minidump to cover the active call depth.

If any of the above is missing, the on-target tombstone is partially or fully unreliable for
the affected frames. The on-host analysis is unaffected by the absence of frame pointers, as
long as `.sym` files exist.

Note: glibc vs. musl does **not** affect on-target signal frame recovery. On-target uses
frame pointer walking only, which cannot cross a signal frame boundary regardless of libc —
`rbp` inside the handler does not chain through `ucontext_t` to the interrupted thread. The
libc choice matters only for on-host CFI-based unwinding; see below.

### On-host analysis requires

1. **`.sym` files for every library** in the call chain, generated by `dump_syms` from the
   unstripped binary and uploaded to the symbol store keyed by build ID.
2. **`-fno-asynchronous-unwind-tables` must not be set** — or if it is, `.sym` files will
   have `FUNC` records but no `STACK CFI`, and the walk degrades to frame pointer + scan for
   that library.
3. **glibc** (not musl) if crashes inside signal handlers must expose the pre-signal call
   chain. glibc's x86-64 sigreturn trampoline (`__restore_rt`, delivered via the vDSO)
   carries CFI describing the `ucontext_t` register layout, so `minidump-stackwalk` can
   follow CFI through the signal frame boundary
   ([MaskRay, 2022](https://maskray.me/blog/2022-04-10-unwinding-through-signal-handler)).
   Musl's trampoline has no CFI; the walk halts at the signal frame in both analyses.
4. **The same minidump quality requirements as on-target** — capture limits, signal frame
   constraints, and stack corruption affect both.

For libraries with no `.sym` file, on-host analysis falls back to frame pointer walking for
those frames, with the same chain-completeness requirement as on-target.

---

## How the two analyses relate

**A frame the on-target walk gets wrong, the on-host walk may get right** — if a `.sym` file
is available for that library. On-target breaks at an assembly function; on-host may have CFI
for the surrounding C code that correctly skips over the assembly gap. The on-host analysis is
always at least as accurate as the on-target analysis for any given library, and usually more
accurate.

**A frame the on-host walk gets wrong, the on-target walk also gets wrong.** If a library has
no `.sym` file and no frame pointers, both analyses fail at the same point. The on-host walk
produces addresses where it can (from `FUNC` records if the library at least has debug info),
but cannot reconstruct the frame chain through that library.

**The on-target tombstone is a triage artifact, not an authoritative trace.** Its value is
immediacy — it appears in journald at crash time without requiring any external infrastructure.
It identifies the crashing module and the approximate call depth. The on-host analysis is
the one to use for root cause investigation.

**A divergence between the two traces is diagnostic information.** If the on-target tombstone
shows frames A→B→C and the on-host analysis shows A→B→inlined_helper→C, the difference is
inlining. If on-target shows A→C (skipping B), B is likely an assembly function or compiled
without frame pointers. If on-target terminates at frame N and on-host continues further, N
was the last frame with a valid frame pointer chain on-target but had CFI coverage on-host.

---

## Summary matrix

### On-target (frame pointer + scan only)

| Condition | Result |
|---|---|
| All libraries compiled with `-fno-omit-frame-pointer` | Accurate walk, addresses only |
| Any library in chain missing frame pointers | Chain breaks at that frame; scan takes over |
| Crash in function prologue/epilogue | Current frame misidentified |
| Crash in PLT stub | Frame pointer not set; scan takes over |
| Assembly function in chain (glibc `memcpy`, etc.) | Frame skipped or walk halts |
| Crash in signal handler (glibc x86-64) | Walk halts at signal frame |
| Crash in signal handler (musl) | Walk halts at signal frame |
| Crash in signal handler (AArch64, any libc) | Walk halts at signal frame |
| Tail-called function | Frame absent; not recoverable |
| Inlined function | Frame absent; not recoverable |

### On-host (CFI primary, frame pointer fallback)

| Condition | Result |
|---|---|
| `.sym` file present for all libraries | Accurate walk, fully symbolicated, inlines expanded |
| No `.sym` for a library, has frame pointers | Falls back to frame pointer for those frames |
| No `.sym` for a library, no frame pointers | Falls back to scan for those frames |
| `-fno-asynchronous-unwind-tables` | No CFI in `.sym`; falls back to frame pointer |
| Assembly function, no CFI annotations | Gap in CFI; frame pointer fallback if `rbp` is not used as a GP register in the assembly — walk halts if `rbp` is clobbered ([Red Hat, 2024](https://developers.redhat.com/articles/2024/10/30/limitations-frame-pointer-unwinding)) |
| PLT stub with no CFI | Gap in CFI; frame pointer fallback for PLT transition |
| Crash in signal handler (glibc x86-64) | CFI crosses signal frame correctly |
| Crash in signal handler (musl) | Halts at signal frame |
| Crash in signal handler (AArch64, any libc) | Halts at signal frame; DWARF cannot simultaneously recover both PC and LR through `ucontext_t` ([MaskRay, 2022](https://maskray.me/blog/2022-04-10-unwinding-through-signal-handler)) |
| Tail-called function | Frame absent; not recoverable by any method |
| Inlined function, `.sym` present | Reconstructed via `DW_TAG_inlined_subroutine` |
| Inlined function, no `.sym` | Frame absent; not recoverable |

### Shared (both analyses)

| Condition | Effect on both |
|---|---|
| Stack deeper than captured memory | Truncated at capture boundary |
| Corrupted stack at crash time | Garbage or early termination |
| Crash in signal handler (musl) | Walk halts at signal frame in both |
| Crash in signal handler (AArch64, any libc) | Walk halts at signal frame in both |
| Tail-called function | Absent from both |

---

## Recommendations

1. **Compile the entire sysroot with `-fno-omit-frame-pointer`** (Yocto/Buildroot global
   `CFLAGS`). This is the minimum requirement for on-target tombstones to be useful. It also
   provides a fallback for on-host analysis of libraries without `.sym` files.

2. **Run `dump_syms` before stripping, upload to the symbol store by build ID.** This is the
   on-host requirement. Without it, on-host analysis degrades to the same quality as on-target.

3. **Use glibc, not musl**, if the pre-signal call chain must be visible in **on-host**
   analysis. glibc's x86-64 sigreturn trampoline carries CFI, allowing `minidump-stackwalk`
   to cross the signal frame boundary. Musl's trampoline lacks CFI; the walk halts at the
   signal frame in both analyses. On-target analysis halts at the signal frame regardless of
   libc, because it uses frame pointer walking only.

4. **Do not set `-fno-asynchronous-unwind-tables`** unless the binary size constraint is
   severe. It disables `.eh_frame` entirely, eliminating CFI from the on-host analysis.

5. **Treat scan-recovered frames as unreliable** in both analyses. The on-target tombstone
   cannot mark them as such (no symbol information to detect when scanning started), but the
   on-host `minidump-stackwalk` output labels each frame's recovery method; frames marked
   `SCAN` should be treated as hints only.

6. **A divergence between on-target and on-host traces is a signal**, not noise. It identifies
   exactly which libraries are missing frame pointers (if on-target breaks earlier) or `.sym`
   files (if on-host lacks CFI for certain frames).

---

## See also

- [frame_pointer_aarch64.md](frame_pointer_aarch64.md) — frame record layout, prologue
  sequence, leaf function behavior, register pressure cost, and signal frame limitations
  specific to AArch64.
