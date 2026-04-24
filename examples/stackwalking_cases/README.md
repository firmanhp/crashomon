# Stackwalking Cases

Examples that exercise every stack-unwinding strategy used by **rust-minidump**
(`minidump-unwind` 0.26.x) on Linux x86-64.  Each case is a small C program
compiled to make certain trust levels appear in the symbolicated output.

---

## Trust levels

rust-minidump assigns a *trust* value to every frame it recovers.  The trust of
frame N reflects the method used to find frame N **from its callee** (frame N−1):

| Trust           | `--show-trust` output     | Meaning |
|-----------------|--------------------------|---------|
| `context`       | `[trust:context]`        | Frame 0 only — registers from the crash context |
| `cfi`           | `[trust:cfi]`            | Recovered via DWARF CFI (`.eh_frame` / STACK CFI in `.sym`) |
| `frame_pointer` | `[trust:frame_pointer]`  | Recovered via `rbp`/`rsp` frame-pointer chain |
| `scan`          | `[trust:scan]` / `[HEURISTIC]` | Heuristic stack scan — unreliable, may produce spurious frames |
| `cfi_scan`      | *(never assigned)*        | Defined in the source but never used on Linux x86-64 |
| `prewalked`     | *(never assigned)*        | Set by an external walker (Windows only) |
| `none`          | *(serialized as `"non"`)* | Placeholder initial value, never written to output |

**Achievable on Linux x86-64: `context`, `cfi`, `frame_pointer`, `scan`.**

The remaining three (`cfi_scan`, `prewalked`, `none`) are defined in the
`FrameTrust` enum but are never assigned by the amd64 or aarch64 stackwalker in
minidump-unwind 0.26.x.

### Trust priority

The stackwalker tries methods in this order for each frame (amd64):
1. CFI — looks up STACK CFI in the `.sym` file for the callee's module+offset
2. Frame pointer — reads `[rbp+0]` (saved caller `rbp`) and `[rbp+8]` (return address)
3. Scan — searches the stack for values that follow a `CALL` instruction

The first method that produces a plausible result wins.

### Why `raise(SIGSEGV)` instead of a null pointer dereference

These examples use `raise(SIGSEGV)` to force a crash.  A direct null pointer
write (`*(volatile int *)0 = 1`) is undefined behaviour in C.  GCC at `-O1` or
higher performs whole-translation-unit UB analysis and silently eliminates every
call in the chain leading to the UB, leaving an empty call chain.
`raise(SIGSEGV)` is an external library call with an observable side effect, so
the compiler cannot remove the chain.

The trade-off: the crash happens inside libc (`raise` → `kill` syscall) rather
than directly in `crash_here`.  The topmost frames are libc frames; the first
user frame (`crash_here`) is recovered with `cfi` trust (because libc has full
`.eh_frame`).  The user-controlled trust levels start from `crash_here`'s
callee (`level_d` / `scan_crash`).

### Why `objcopy` strips CFI for the frame-pointer and scan cases

GCC emits `.eh_frame` (runtime unwind) and `.debug_frame` (debug unwind)
even with `-fno-asynchronous-unwind-tables -fno-unwind-tables`.  dump_syms
reads both sections when generating STACK CFI records, so compile flags alone
cannot suppress them.  The Makefiles for cases 02, 03, and the `fp_part`/
`scan_part` objects in 04 run:

```
objcopy --remove-section=.eh_frame \
        --remove-section=.eh_frame_hdr \
        --remove-section=.debug_frame
```

This gives a binary with full debug symbols (FUNC/LINE in the `.sym`) but zero
STACK CFI records, forcing the desired trust-level fallback.

---

## Cases

### `01_all_cfi` — CFI trust throughout

All functions compiled with `-g -fasynchronous-unwind-tables -fno-omit-frame-pointer
-fno-optimize-sibling-calls`.  Every function has complete `.eh_frame` coverage
including prologue body records; dump_syms writes STACK CFI for all of them.

| Frame | Function    | Trust   |
|-------|-------------|---------|
| #0    | raise/kill  | context |
| #1    | crash_here  | cfi     |
| #2    | level_d     | cfi     |
| #3    | level_c     | cfi     |
| #4    | level_b     | cfi     |
| #5    | level_a     | cfi     |
| #6    | main        | cfi     |

### `02_frame_pointer` — frame-pointer trust throughout

All functions compiled with `-g -fno-omit-frame-pointer -fno-optimize-sibling-calls`.
After compilation the Makefile strips `.eh_frame`/`.debug_frame` with objcopy.  The
`.sym` has FUNC/LINE records but no STACK CFI.  The stackwalker falls back to
the `rbp` chain.

| Frame | Function    | Trust          |
|-------|-------------|----------------|
| #0    | raise/kill  | context        |
| #1    | crash_here  | cfi            |
| #2    | level_d     | frame_pointer  |
| #3    | level_c     | frame_pointer  |
| #4    | level_b     | frame_pointer  |
| #5    | level_a     | frame_pointer  |
| #6    | main        | frame_pointer  |

Note: frame #1 (`crash_here`) gets `cfi` trust because it is recovered from
the libc `raise` frame using libc's `.eh_frame`, not from the user `.sym`.
The user-binary trust levels begin at frame #2 and beyond.

### `03_scan` — scan trust throughout

All functions compiled with `-g -fomit-frame-pointer -fno-optimize-sibling-calls`.
Makefile strips `.eh_frame`/`.debug_frame` with objcopy.  No CFI, no `rbp` chain.
`-fno-optimize-sibling-calls` ensures every call is a `CALL` instruction so
return addresses stay on the stack for scanning.

> **Warning:** scan is heuristic.  The recovered frames are plausible but not
> guaranteed correct.  Extra frames or wrong function names are possible.

| Frame | Function    | Trust   |
|-------|-------------|---------|
| #0    | raise/kill  | context |
| #1    | crash_here  | cfi     |
| #2    | level_d     | scan    |
| #3    | level_c     | scan    |
| #4    | level_b     | scan    |
| #5    | level_a     | scan    |
| #6    | main        | scan    |

### `04_mixed` — all three non-context trust levels in one trace


Four source files compiled with different flags, linked into one binary.  The
call chain crosses the compilation boundaries:

```
main              (CFI)
  cfi_relay_outer (CFI)
    cfi_relay_inner (CFI)
      fp_relay_outer  (frame pointer, no CFI — objcopy stripped)
        fp_relay_inner  (frame pointer, no CFI — objcopy stripped)
          scan_relay      (no FP, no CFI — objcopy stripped)
            scan_crash      (no FP, no CFI — crash site, calls raise)
```

| Frame | Function        | Trust          | Why |
|-------|-----------------|----------------|-----|
| #0    | raise/kill      | context        | crash registers |
| #1    | scan_crash      | cfi            | recovered via libc .eh_frame |
| #2    | scan_relay      | scan           | `scan_crash`: no FP, no CFI in .sym |
| #3    | fp_relay_inner  | scan           | `scan_relay`: no FP, no CFI in .sym |
| #4    | fp_relay_outer  | frame_pointer  | `fp_relay_inner`: FP, no CFI |
| #5    | cfi_relay_inner | frame_pointer  | `fp_relay_outer`: FP, no CFI |
| #6    | cfi_relay_outer | cfi            | `cfi_relay_inner` has STACK CFI |
| #7    | main            | cfi            | `cfi_relay_outer` has STACK CFI |

The trust transition `scan` → `frame_pointer` at frame #4 and
`frame_pointer` → `cfi` at frame #6 are the key demonstration points.

### `05_portable_explicit` — compiler-independent explicit flags

Demonstrates that unwind behaviour can be made **reproducible across GCC and
Clang** by using specific behaviour flags instead of optimization levels.  The
same source is compiled three times:

| Binary        | Flags added beyond base (`-g -fno-optimize-sibling-calls`) |
|---------------|-------------------------------------------------------------|
| `crasher_cfi`  | `-fasynchronous-unwind-tables -fno-omit-frame-pointer`      |
| `crasher_fp`   | `-fno-omit-frame-pointer` + objcopy CFI strip               |
| `crasher_scan` | `-fomit-frame-pointer` + objcopy CFI strip                  |

Each binary produces the same trust sequence as its single-binary equivalent
(case 01, 02, and 03 respectively) regardless of which compiler is used.

The Makefile uses `CC ?= gcc` so you can override: `make CC=clang`.

Expected trust sequences — identical for both GCC and Clang:

**`crasher_cfi`:**

| Frame | Function   | Trust   |
|-------|------------|---------|
| #0    | raise/kill | context |
| #1    | crash_here | cfi     |
| #2–#6 | level_d…main | cfi  |

**`crasher_fp`:**

| Frame | Function   | Trust          |
|-------|------------|----------------|
| #0    | raise/kill | context        |
| #1    | crash_here | cfi            |
| #2–#6 | level_d…main | frame_pointer |

**`crasher_scan`:**

| Frame | Function   | Trust   |
|-------|------------|---------|
| #0    | raise/kill | context |
| #1    | crash_here | cfi     |
| #2–#6 | level_d…main | scan  |

### `06_optimization_levels` — practical developer builds

Shows what trust levels developers actually get when they use standard `-Ox`
flags without any extra unwind configuration.

On x86-64 Linux, `-fasynchronous-unwind-tables` is the **ABI default** at
every optimization level: GCC and Clang always emit `.eh_frame`.  With a `.sym`
file present, every frame resolves with `cfi` trust regardless of `-Ox`.

The frame pointer is a different story:

| Flag | GCC default              | Effect without `.sym`           |
|------|--------------------------|---------------------------------|
| `-O0` | `-fno-omit-frame-pointer` | falls back to `frame_pointer`   |
| `-O1` | `-fomit-frame-pointer`   | falls back to `scan` (heuristic)|
| `-O2` | `-fomit-frame-pointer`   | falls back to `scan` (heuristic)|
| `-O3` | `-fomit-frame-pointer`   | falls back to `scan` (heuristic)|

The binaries `crasher_O0`, `crasher_O1`, `crasher_O2`, `crasher_O3` each
carry full `.eh_frame` (no stripping).  The expected sequences **with `.sym`**:

| Frame | Function   | Trust (all four binaries) |
|-------|------------|---------------------------|
| #0    | raise/kill | context                   |
| #1    | crash_here | cfi                       |
| #2–#6 | level_d…main | cfi                    |

The practical takeaway: if you deploy symbols (via `crashomon-syms add`), the
optimization level does not affect stack-trace quality.  If symbols are missing,
`-O1` and above lose the frame pointer chain and fall back to heuristic scanning.

---

## Actual observed output

Pre-captured traces from all cases are in [`sample_traces.md`](sample_traces.md).
Each entry includes the real `crashomon-analyze --show-trust` output, the
compiler flags, symbol record counts, and annotations explaining surprising
behaviour (missing frames, unexpected trust values, scan noise).

---

## Build and run

### 1. Build the project

```bash
# From the repo root:
cmake -B _dump_syms_build -S cmake/dump_syms_host/
cmake --build _dump_syms_build -j$(nproc)

cmake -B build \
    -DCRASHOMON_DUMP_SYMS_EXECUTABLE="$(pwd)/_dump_syms_build/dump_syms" \
    -DCRASHOMON_BUILD_ANALYZE=ON
cmake --build build -j$(nproc)
```

### 2. Run all cases

```bash
examples/stackwalking_cases/generate_traces.sh
```

The script builds each example, crashes it under Crashpad, and runs
`crashomon-analyze --show-trust` to annotate every frame.

Custom binary paths:

```bash
examples/stackwalking_cases/generate_traces.sh \
    --dump-syms  /path/to/dump_syms \
    --stackwalk  /path/to/minidump-stackwalk
```

### 3. Run a single case manually

```bash
cd examples/stackwalking_cases/01_all_cfi
make

# Extract symbols
DUMP_SYMS=../../../_dump_syms_build/dump_syms
$DUMP_SYMS crasher > crasher.sym

# Crash under Crashpad (watcherd must already be running)
LD_PRELOAD=../../../build/lib/libcrashomon.so \
    CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock \
    ./crasher

# Symbolicate with trust annotations
../../../tools/analyze/crashomon-analyze \
    --symbols=crasher.sym \
    --minidump=/path/to/captured.dmp \
    --stackwalk-binary=../../../build/rust_minidump_target/release/minidump-stackwalk \
    --show-trust
```

---

## The `--show-trust` flag

`crashomon-analyze --show-trust` appends `[trust:XXX]` to every frame.
Without `--show-trust`, only scan-recovered frames get the `[HEURISTIC]` marker.

Example output from case 04 (04_mixed):

```
backtrace:
    #00 pc 0x00001060  libc.so.6  (raise) [trust:context]
    #01 pc 0x000011d0  crasher    (scan_crash) [trust:cfi]
    #02 pc 0x000011f0  crasher    (scan_relay) [trust:scan]
    #03 pc 0x000011b0  crasher    (fp_relay_inner) [trust:scan]
    #04 pc 0x000011c0  crasher    (fp_relay_outer) [trust:frame_pointer]
    #05 pc 0x00001190  crasher    (cfi_relay_inner) [trust:frame_pointer]
    #06 pc 0x000011a0  crasher    (cfi_relay_outer) [trust:cfi]
    #07 pc 0x00001060  crasher    (main) [trust:cfi]
```
