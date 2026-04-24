# Stack Walking Takeaways

Conclusions drawn from the six cases in this directory.  All observations are
from actual minidump captures on x86-64 Linux (GCC 11, glibc 2.35) — see
[`sample_traces.md`](sample_traces.md) for the raw output.

---

## The one lever that matters most: deploy symbols

Case 06 makes this concrete.  `-O2` and `-O3` both omit the frame pointer by
default, yet all four `-Ox` binaries produce **identical, complete `cfi` traces**
as long as a `.sym` file is present.

Why it works: on x86-64 Linux, `-fasynchronous-unwind-tables` is an ABI default
at every optimization level.  The compiler always emits `.eh_frame`.  `dump_syms`
converts it to STACK CFI records.  The stackwalker reads those records and
recovers every frame with `cfi` trust regardless of how the binary was optimized.

**Run `crashomon-syms add` in CI after every build.  That is the only reliable
path to a complete, trustworthy stack trace.**

---

## What you can control

### 1. Symbol deployment (highest priority)

Deploy symbols for every binary you care about.  Compile flags become irrelevant
to stack trace quality once symbols are present.

### 2. Frame pointer as a no-symbols fallback

If symbols genuinely cannot be deployed (device has no connectivity, binary size
constraints, third-party libraries), compiling with `-fno-omit-frame-pointer` is
the only alternative that still produces a complete trace.

Cases 02 and `crasher_fp` (case 05) show the full user call chain via
`frame_pointer` trust with zero STACK CFI records.  The chain is complete and
every function is named correctly.

The cost on x86-64: roughly 1–2% performance due to one fewer register available
to the allocator.  This is the usual trade-off for `-fno-omit-frame-pointer` in
production builds.

The frame pointer is a fallback, not a substitute for symbols.  It gives you a
complete chain; it does not give you source file and line number on the frames.

---

## What you cannot control

### 1. The first user frame above a libc crash always gets `frame_pointer` trust

In every case — including case 01 with 29 STACK CFI records — the function that
called `raise()` is recovered with `frame_pointer` trust, not `cfi`.

The reason: trust of frame N is determined by what unwinding information is
available in frame N-1 (the callee).  When the callee is a libc internal frame
and no libc sym file is provided, the walker uses libc's own frame pointer chain.
That trust value is assigned to your first user frame.

You will never see `cfi` on the frame immediately above a libc crash boundary
unless you also deploy libc symbols.  In practice this is one frame and does not
affect the usefulness of the trace.

### 2. Frames inside a frameless, no-CFI zone are permanently gone

Case 04 is the proof.  The intended call chain is eight frames deep.  Two of
them — `scan_relay` and `fp_relay_inner` — are compiled without a frame pointer
and without CFI.  They are absent from the trace entirely.

The stackwalker cannot traverse a region it has no unwind information for.  Even
if everything above the gap has perfect CFI, the functions inside the gap are
unrecoverable.  The trace shows the frames above the gap and the frames below it,
with no indication that anything is missing unless you notice the discontinuity.

**Implication for mixed codebases:** a single third-party library compiled
without frame pointers and without deployed symbols creates a hole in every stack
trace that passes through it.

### 3. Scan noise appears at the end of every trace

Every case — including the best-case all-CFI binary — ends with spurious
`[trust:scan]` frames after the valid stack terminates.  The scanner continues
past the bottom of the thread stack and finds stale values that look like return
addresses.  Case 02 produces four false-positive scan frames; case 01 produces
two.

You cannot prevent this.  You can identify and discard these frames using
`--show-trust` (every scan frame is labelled) or by looking for `[HEURISTIC]`
in default output.  Any frame with `[trust:scan]` after a plausible
`__libc_start_main` should be treated as noise.

---

## Priority order for a crashomon deployment

| Priority | Action | What you get |
|----------|--------|--------------|
| 1 | Run `crashomon-syms add` in CI | `cfi` trust, complete chain, source lines, any `-Ox` |
| 2 | Build with `-fno-omit-frame-pointer` | `frame_pointer` trust, complete chain, no source lines |
| 3 | Neither of the above | `scan` trust, incomplete chain, false positives |

There is no combination of compiler flags that substitutes for deployed symbols.
The flags control what happens when symbols are absent; symbols eliminate the
problem entirely.

---

## Reading a trace with `--show-trust`

| Trust value you see | Interpretation |
|---------------------|----------------|
| `context` | Frame 0, always reliable — these are the crash registers |
| `cfi` | Reliable — DWARF CFI from the `.sym` file |
| `frame_pointer` | Reliable — `rbp` chain intact; function names are correct |
| `scan` | Unreliable — treat each scan frame as a hint, not a fact |

When a trace transitions from `cfi` or `frame_pointer` into `scan`, the frames
from that point on are suspect.  Some may be correct; some are false positives.
The valid trace effectively ends at the last non-scan frame.

The `scan` frames produced by case 03 (`crasher_scan`) illustrate the worst
case: three out of five expected intermediate functions are simply absent, one
false positive (`level_a`) appears, and the remaining scan frames are unrelated
to the actual call chain.
