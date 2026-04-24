# Sample Traces — Actual Observed Output

Captured on x86-64 Linux (kernel 6.17), GCC 11, glibc 2.35.
Built with `cmake -DCRASHOMON_BUILD_ANALYZE=ON`, symbolicated with
`crashomon-analyze --show-trust` using only the user-binary `.sym` file
(no libc symbols — the default for embedded deployments).

The trust label on **frame N** is the method used to *recover* frame N from
its callee (frame N−1), not how frame N itself was compiled.

---

## Notation

| Marker           | Meaning |
|------------------|---------|
| `[trust:context]`       | Frame 0 — registers from crash context |
| `[trust:cfi]`           | Recovered via DWARF CFI from `.sym` |
| `[trust:frame_pointer]` | Recovered via `rbp` chain |
| `[trust:scan]`          | Heuristic stack scan — may be wrong |
| `[HEURISTIC]`           | Same as scan, shown without `--show-trust` |

---

## Case 01 — `01_all_cfi`

**Compiler flags:** `-g -fasynchronous-unwind-tables -fno-omit-frame-pointer -fno-optimize-sibling-calls`
**Symbol records:** 6 FUNC, 29 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 520771, tid: 520771, name: crasher  >>> crasher <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f243

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher  (crash_here) [crasher.c:27] [trust:frame_pointer]
    #03 pc 0x0000118a  crasher  (level_d) [crasher.c:28] [trust:cfi]
    #04 pc 0x0000119a  crasher  (level_c) [crasher.c:29] [trust:cfi]
    #05 pc 0x000011aa  crasher  (level_b) [crasher.c:30] [trust:cfi]
    #06 pc 0x000011ba  crasher  (level_a) [crasher.c:31] [trust:cfi]
    #07 pc 0x000011d9  crasher  (main) [crasher.c:35] [trust:cfi]
    #08 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher  (_fini) [trust:scan]
    #11 pc 0x000011bd  crasher  (level_a) [crasher.c:31] [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Notes:**

- Frames #0–#1 are inside libc (`raise` → internal kill stub). The trust of
  `crash_here` (#2) is `frame_pointer`, not `cfi`, because trust reflects how
  the frame was *recovered from its callee* (the libc internal stub, frame #1).
  Since no sym file for libc is provided, the walker used libc's own frame
  pointer chain to unwind through libc frames. The user CFI begins at frame #3.

- Frame #8 (libc `__libc_start_main`) gets `cfi` trust because `main`'s sym
  entry provides CFI for main's callee side — the walker found the return
  address from main back into libc using main's STACK CFI records.

- Frames #10–#11 are **spurious scan frames** produced after the frame pointer
  chain terminates at the bottom of the thread stack. Frame #11 (`level_a`) is
  a false positive — the scanner found a value on the stack that happened to
  point into `level_a`. The valid trace ends at frame #9.

---

## Case 02 — `02_frame_pointer`

**Compiler flags:** `-g -fno-omit-frame-pointer -fno-optimize-sibling-calls` + `objcopy` CFI strip
**Symbol records:** 6 FUNC, 0 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 520829, tid: 520829, name: crasher  >>> crasher <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f27d

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher  (crash_here) [crasher.c:25] [trust:frame_pointer]
    #03 pc 0x0000118a  crasher  (level_d) [crasher.c:26] [trust:frame_pointer]
    #04 pc 0x0000119a  crasher  (level_c) [crasher.c:27] [trust:frame_pointer]
    #05 pc 0x000011aa  crasher  (level_b) [crasher.c:28] [trust:frame_pointer]
    #06 pc 0x000011ba  crasher  (level_a) [crasher.c:29] [trust:frame_pointer]
    #07 pc 0x000011d9  crasher  (main) [crasher.c:33] [trust:frame_pointer]
    #08 pc 0x0002a1c9  libc.so.6 [trust:frame_pointer]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher  (_fini) [trust:scan]
    #11 pc 0x000011bd  crasher  (level_a) [crasher.c:29] [trust:scan]
    #12 pc 0x0000107f  crasher  (_init) [trust:scan]
    #13 pc 0x000010a4  crasher  (_start) [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Notes:**

- The full user call chain (`crash_here` through `main`) is intact via the `rbp`
  frame pointer chain. All six user frames appear with `frame_pointer` trust.

- Frame #8 (`__libc_start_main`) is now `frame_pointer` rather than `cfi`
  (compare with case 01): with no STACK CFI in the user sym file, `main`'s
  frame is walked via its frame pointer, not via CFI.

- Frames #10–#13 are spurious scan frames past the bottom of the thread stack.
  The scan produces more false positives here (four frames vs. two in case 01)
  because the frame pointer chain extends further before terminating. Valid
  trace ends at frame #9.

---

## Case 03 — `03_scan`

**Compiler flags:** `-g -fomit-frame-pointer -fno-optimize-sibling-calls` + `objcopy` CFI strip
**Symbol records:** 6 FUNC, 0 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 520895, tid: 520895, name: crasher  >>> crasher <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f2bf

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher  (crash_here) [crasher.c:31] [trust:frame_pointer]
    #03 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #04 pc 0x00003db7  crasher  (_fini) [trust:scan]
    #05 pc 0x000011cc  crasher  (level_a) [crasher.c:35] [trust:scan]
    #06 pc 0x0000107f  crasher  (_init) [trust:scan]
    #07 pc 0x000010a4  crasher  (_start) [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Notes:**

- **`level_d` through `level_b` are missing entirely.** Without the frame
  pointer chain or CFI, the walker cannot reliably find intermediate frames.
  This is the core failure mode of `-fomit-frame-pointer` without symbols.

- `crash_here` (#2) is recovered with `frame_pointer` trust. With no -Ox flag,
  GCC does not honour `-fomit-frame-pointer` for simple leaf-ish functions in
  the absence of optimisation — `crash_here`'s prologue still sets up `rbp`.
  The libc frame pointer chain continues into `crash_here`'s frame.

- `level_a` appears at frame #5 as a **false positive** from the scan. The
  scanner found a value on the stack that resembled a return address into
  `level_a`. It is not trustworthy as a call-chain entry.

- This trace demonstrates why crash reports from scan-only binaries (no debug
  symbols deployed, frame pointer omitted by -O2/-O3) are often incomplete or
  misleading.

---

## Case 04 — `04_mixed`

**Object files:** `main.o` + `cfi_part.o` (full CFI) linked with `fp_part.o`
(frame pointer, CFI stripped) and `scan_part.o` (no FP, CFI stripped)
**Symbol records:** 7 FUNC, 17 STACK CFI

Call chain: `main` → `cfi_relay_outer` → `cfi_relay_inner` → `fp_relay_outer` →
`fp_relay_inner` → `scan_relay` → `scan_crash` → `raise(SIGSEGV)`

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 520987, tid: 520987, name: crasher  >>> crasher <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f31b

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x000011dd  crasher  (scan_crash) [scan_part.c:15] [trust:frame_pointer]
    #03 pc 0x000011b8  crasher  (fp_relay_outer) [fp_part.c:10] [trust:frame_pointer]
    #04 pc 0x000011a8  crasher  (cfi_relay_inner) [cfi_part.c:10] [trust:frame_pointer]
    #05 pc 0x00001198  crasher  (cfi_relay_outer) [cfi_part.c:9] [trust:cfi]
    #06 pc 0x00001184  crasher  (main) [main.c:40] [trust:cfi]
    #07 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #08 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #09 pc 0x00003db7  crasher  (_fini) [trust:scan]
    #10 pc 0x00001168  crasher  (frame_dummy) [trust:scan]
    #11 pc 0x0000107f  crasher  (_init) [trust:scan]
    #12 pc 0x00000000  ??? [trust:cfi]
    #13 pc 0x000010a4  crasher  (_start) [trust:scan]
    #14 pc 0x00000000  ??? [trust:cfi]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Notes:**

- **`scan_relay` and `fp_relay_inner` are absent** — both compiled without a
  frame pointer; the walker cannot traverse the no-FP segment of the chain.
  `fp_relay_outer` is recovered directly from `scan_crash`'s frame (GCC at
  no -Ox level doesn't fully omit `rbp`, as noted in case 03; the libc frame
  pointer chain extends through `scan_crash` and one more hop lands on
  `fp_relay_outer` which has a real frame pointer).

- The `frame_pointer` → `cfi` transition is visible at frame #5:
  `cfi_relay_outer` is recovered via CFI (its callee, `cfi_relay_inner`, has
  STACK CFI in the sym file).

- Frame #4 (`cfi_relay_inner`) has `frame_pointer` trust because its callee
  (`fp_relay_outer`) lacks CFI; the walker used the rbp chain to find it.

- Frames #9–#14 are spurious scan frames and null-pointer `cfi` artefacts past
  the bottom of the valid stack. Valid trace ends at frame #8.

---

## Case 05 — `05_portable_explicit`

Same source, three binaries with explicit compiler-agnostic flags.
`CC ?= gcc` — override with `make CC=clang` for identical results.

### `crasher_cfi`

**Flags:** `-g -fasynchronous-unwind-tables -fno-omit-frame-pointer -fno-optimize-sibling-calls`
**Symbol records:** 6 FUNC, 29 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521057, tid: 521057, name: crasher_cfi  >>> crasher_cfi <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f361

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher_cfi  (crash_here) [crasher.c:42] [trust:frame_pointer]
    #03 pc 0x0000118a  crasher_cfi  (level_d) [crasher.c:43] [trust:cfi]
    #04 pc 0x0000119a  crasher_cfi  (level_c) [crasher.c:44] [trust:cfi]
    #05 pc 0x000011aa  crasher_cfi  (level_b) [crasher.c:45] [trust:cfi]
    #06 pc 0x000011ba  crasher_cfi  (level_a) [crasher.c:46] [trust:cfi]
    #07 pc 0x000011d9  crasher_cfi  (main) [crasher.c:50] [trust:cfi]
    #08 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher_cfi  (_fini) [trust:scan]
    #11 pc 0x000011bd  crasher_cfi  (level_a) [crasher.c:46] [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

### `crasher_fp`

**Flags:** `-g -fno-omit-frame-pointer -fno-optimize-sibling-calls` + `objcopy` CFI strip
**Symbol records:** 6 FUNC, 0 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521105, tid: 521105, name: crasher_fp  >>> crasher_fp <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f391

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher_fp  (crash_here) [crasher.c:42] [trust:frame_pointer]
    #03 pc 0x0000118a  crasher_fp  (level_d) [crasher.c:43] [trust:frame_pointer]
    #04 pc 0x0000119a  crasher_fp  (level_c) [crasher.c:44] [trust:frame_pointer]
    #05 pc 0x000011aa  crasher_fp  (level_b) [crasher.c:45] [trust:frame_pointer]
    #06 pc 0x000011ba  crasher_fp  (level_a) [crasher.c:46] [trust:frame_pointer]
    #07 pc 0x000011d9  crasher_fp  (main) [crasher.c:50] [trust:frame_pointer]
    #08 pc 0x0002a1c9  libc.so.6 [trust:frame_pointer]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher_fp  (_fini) [trust:scan]
    #11 pc 0x000011bd  crasher_fp  (level_a) [crasher.c:46] [trust:scan]
    #12 pc 0x0000107f  crasher_fp  (_init) [trust:scan]
    #13 pc 0x000010a4  crasher_fp  (_start) [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

### `crasher_scan`

**Flags:** `-g -fomit-frame-pointer -fno-optimize-sibling-calls` + `objcopy` CFI strip
**Symbol records:** 6 FUNC, 0 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521164, tid: 521164, name: crasher_scan  >>> crasher_scan <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f3cc

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher_scan  (crash_here) [crasher.c:42] [trust:frame_pointer]
    #03 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #04 pc 0x00003db7  crasher_scan  (_fini) [trust:scan]
    #05 pc 0x000011cc  crasher_scan  (level_a) [crasher.c:46] [trust:scan]
    #06 pc 0x0000107f  crasher_scan  (_init) [trust:scan]
    #07 pc 0x000010a4  crasher_scan  (_start) [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Notes for case 05:**

- `crasher_cfi` and `crasher_fp` produce complete traces — all six user frames
  are present. `crasher_scan` loses `level_d` through `level_b` for the same
  reason as case 03: without a frame pointer or CFI, the intermediate frames
  are unrecoverable.

- The trust transition between the libc boundary and user code is identical
  across all three binaries: libc frames are always `frame_pointer` (no libc
  sym file), and the first user frame (`crash_here`) inherits that. User CFI
  or frame-pointer walking begins at `crash_here`'s callers.

---

## Case 06 — `06_optimization_levels`

Same source compiled four times with standard `-Ox` flags and no explicit
unwind-behaviour overrides. Demonstrates that on x86-64 Linux, `.eh_frame`
(and therefore CFI trust) is the ABI default at every optimization level.

### `crasher_O0`

**Flags:** `-O0 -g -Wall -Wextra -fno-optimize-sibling-calls`
**Symbol records:** 6 FUNC, 29 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521246, tid: 521246, name: crasher_O0  >>> crasher_O0 <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f41e

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher_O0  (crash_here) [crasher.c:39] [trust:frame_pointer]
    #03 pc 0x0000118a  crasher_O0  (level_d) [crasher.c:40] [trust:cfi]
    #04 pc 0x0000119a  crasher_O0  (level_c) [crasher.c:41] [trust:cfi]
    #05 pc 0x000011aa  crasher_O0  (level_b) [crasher.c:42] [trust:cfi]
    #06 pc 0x000011ba  crasher_O0  (level_a) [crasher.c:43] [trust:cfi]
    #07 pc 0x000011d9  crasher_O0  (main) [crasher.c:47] [trust:cfi]
    #08 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher_O0  (_fini) [trust:scan]
    #11 pc 0x000011bd  crasher_O0  (level_a) [crasher.c:43] [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

### `crasher_O1`

**Flags:** `-O1 -g -Wall -Wextra -fno-optimize-sibling-calls`
**Symbol records:** 6 FUNC, 23 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521295, tid: 521295, name: crasher_O1  >>> crasher_O1 <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f44f

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x0000117a  crasher_O1  (crash_here) [crasher.c:39] [trust:frame_pointer]
    #03 pc 0x0000118c  crasher_O1  (level_d) [crasher.c:40] [trust:cfi]
    #04 pc 0x0000119e  crasher_O1  (level_c) [crasher.c:41] [trust:cfi]
    #05 pc 0x000011b0  crasher_O1  (level_b) [crasher.c:42] [trust:cfi]
    #06 pc 0x000011c2  crasher_O1  (level_a) [crasher.c:43] [trust:cfi]
    #07 pc 0x000011e0  crasher_O1  (main) [crasher.c:47] [trust:cfi]
    #08 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher_O1  (_fini) [trust:scan]
    #11 pc 0x000011c7  crasher_O1  (level_a) [crasher.c:43] [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

### `crasher_O2`

**Flags:** `-O2 -g -Wall -Wextra -fno-optimize-sibling-calls`
**Symbol records:** 6 FUNC, 23 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521341, tid: 521341, name: crasher_O2  >>> crasher_O2 <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f47d

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x000011a1  crasher_O2  (crash_here) [crasher.c:39] [trust:frame_pointer]
    #03 pc 0x000011bc  crasher_O2  (level_d) [crasher.c:40] [trust:cfi]
    #04 pc 0x000011dc  crasher_O2  (level_c) [crasher.c:41] [trust:cfi]
    #05 pc 0x000011fc  crasher_O2  (level_b) [crasher.c:42] [trust:cfi]
    #06 pc 0x0000121c  crasher_O2  (level_a) [crasher.c:43] [trust:cfi]
    #07 pc 0x00001098  crasher_O2  (main) [crasher.c:47] [trust:cfi]
    #08 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher_O2  (_fini) [trust:scan]
    #11 pc 0x0000107f  crasher_O2  (_init) [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

### `crasher_O3`

**Flags:** `-O3 -g -Wall -Wextra -fno-optimize-sibling-calls`
**Symbol records:** 6 FUNC, 23 STACK CFI

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
pid: 521399, tid: 521399, name: crasher_O3  >>> crasher_O3 <<<
signal 11 (SIGSEGV), code 0 (SI_TKILL), fault addr 0x3e80007f4b7

backtrace:
    #00 pc 0x0009eb2c  libc.so.6 [trust:context]
    #01 pc 0x0004527d  libc.so.6 [trust:frame_pointer]
    #02 pc 0x000011a1  crasher_O3  (crash_here) [crasher.c:39] [trust:frame_pointer]
    #03 pc 0x000011bc  crasher_O3  (level_d) [crasher.c:40] [trust:cfi]
    #04 pc 0x000011dc  crasher_O3  (level_c) [crasher.c:41] [trust:cfi]
    #05 pc 0x000011fc  crasher_O3  (level_b) [crasher.c:42] [trust:cfi]
    #06 pc 0x0000121c  crasher_O3  (level_a) [crasher.c:43] [trust:cfi]
    #07 pc 0x00001098  crasher_O3  (main) [crasher.c:47] [trust:cfi]
    #08 pc 0x0002a1c9  libc.so.6 [trust:cfi]
    #09 pc 0x0002a28a  libc.so.6 [trust:frame_pointer]
    #10 pc 0x00003db7  crasher_O3  (_fini) [trust:scan]
    #11 pc 0x0000107f  crasher_O3  (_init) [trust:scan]
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
```

**Notes for case 06:**

- **All four optimization levels produce identical trust sequences** (`cfi`
  throughout the user call chain). This is because `-fasynchronous-unwind-tables`
  is the ABI default on x86-64 Linux at every `-Ox` level — the compiler always
  emits `.eh_frame`, and `dump_syms` always converts it to STACK CFI records.

- STACK CFI record count drops from 29 (`-O0`) to 23 (`-O1`/`-O2`/`-O3`).
  At `-O0` GCC emits a CFI body record for every `push`/`mov` in the prologue.
  At `-O1` and above the frame pointer is omitted (`-fomit-frame-pointer` is
  a default), so the prologue is shorter and requires fewer CFI records to
  describe. The CFI is still accurate and complete — just more compact.

- The **practical takeaway**: deploying debug symbols with `crashomon-syms add`
  gives full `cfi`-quality stack traces at any optimization level. Without
  symbols, `-O1`/`-O2`/`-O3` lose the frame pointer (`rbp`) and fall back to
  heuristic scan (see case 03 for what that looks like in practice).

---

## Summary

| Case | Binary | FUNC | STACK CFI | User chain complete? | Dominant trust |
|------|--------|-----:|----------:|:--------------------:|----------------|
| 01 | `crasher` | 6 | 29 | yes | cfi (from level_d up) |
| 02 | `crasher` | 6 | 0 | yes | frame_pointer |
| 03 | `crasher` | 6 | 0 | **no** (missing level_d–level_b) | scan |
| 04 | `crasher` | 7 | 17 | **no** (missing scan_relay, fp_relay_inner) | mixed |
| 05 | `crasher_cfi` | 6 | 29 | yes | cfi |
| 05 | `crasher_fp` | 6 | 0 | yes | frame_pointer |
| 05 | `crasher_scan` | 6 | 0 | **no** | scan |
| 06 | `crasher_O0` | 6 | 29 | yes | cfi |
| 06 | `crasher_O1` | 6 | 23 | yes | cfi |
| 06 | `crasher_O2` | 6 | 23 | yes | cfi |
| 06 | `crasher_O3` | 6 | 23 | yes | cfi |
