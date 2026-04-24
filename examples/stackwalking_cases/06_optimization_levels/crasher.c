/*
 * Case 06: practical optimization levels
 *
 * The same source file is compiled four times, one per -Ox level, with no
 * extra unwind-behaviour flags — exactly what a developer gets in a typical
 * build system.
 *
 *   crasher_O0  — -O0 -g
 *   crasher_O1  — -O1 -g
 *   crasher_O2  — -O2 -g
 *   crasher_O3  — -O3 -g
 *
 * On x86-64 Linux, -fasynchronous-unwind-tables is an ABI default at every
 * optimization level: GCC/Clang always emits .eh_frame.  dump_syms converts
 * that into STACK CFI records.  As long as the .sym file is present, every
 * frame resolves with cfi trust regardless of -Ox.
 *
 * The frame pointer behaviour DOES change:
 *   -O0           : frame pointer kept (-fno-omit-frame-pointer implicitly)
 *   -O1 and above : -fomit-frame-pointer enabled (GCC default on x86-64)
 *
 * Without a .sym file (e.g. no debug symbols deployed to the symbol server):
 *   crasher_O0  — frame_pointer  (rbp chain intact)
 *   crasher_O1/O2/O3 — scan      (rbp chain gone; heuristic only)
 *
 * Expected trust sequences (WITH .sym file — the normal case):
 *
 *   All four binaries:
 *     #0  raise/kill   — context
 *     #1  crash_here   — cfi
 *     #2  level_d      — cfi
 *     ...
 *     #6  main         — cfi
 */

#include <signal.h>
#include <stdio.h>

void __attribute__((noinline)) crash_here(void) { raise(SIGSEGV); }
void __attribute__((noinline)) level_d(void) { crash_here(); }
void __attribute__((noinline)) level_c(void) { level_d(); }
void __attribute__((noinline)) level_b(void) { level_c(); }
void __attribute__((noinline)) level_a(void) { level_b(); }

int main(void) {
    puts("06_optimization_levels: same source, four -Ox levels");
    level_a();
    return 0;
}
