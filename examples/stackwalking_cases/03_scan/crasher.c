/*
 * Case 03: stack scan only
 *
 * Compiled with -g -fomit-frame-pointer.  After compilation the Makefile
 * strips .eh_frame, .eh_frame_hdr, and .debug_frame with objcopy, so the .sym
 * has function names (from .debug_info) but no STACK CFI and no rbp chain.
 * The stackwalker falls back to heuristic stack scanning.
 *
 * -fno-optimize-sibling-calls is essential: it prevents tail-call elimination
 * so every call uses CALL (not JMP), keeping all return addresses on the stack
 * for the scanner to find.
 *
 * raise(SIGSEGV) is used instead of a null-pointer dereference — see case 01
 * for the reason.
 *
 * Expected trust sequence (bottom to top):
 *   #0  raise / kill   — context   (crash register state inside libc)
 *   #1  crash_here     — cfi       (libc has full .eh_frame)
 *   #2  level_d        — scan      (crash_here: no FP, no CFI in .sym)  [HEURISTIC]
 *   #3  level_c        — scan      [HEURISTIC]
 *   #4  level_b        — scan      [HEURISTIC]
 *   #5  level_a        — scan      [HEURISTIC]
 *   #6  main           — scan      [HEURISTIC]
 *
 * Note: scan recovery is heuristic.  Spurious extra frames are possible.
 */

#include <signal.h>
#include <stdio.h>

void __attribute__((noinline)) crash_here(void) { raise(SIGSEGV); }
void __attribute__((noinline)) level_d(void) { crash_here(); }
void __attribute__((noinline)) level_c(void) { level_d(); }
void __attribute__((noinline)) level_b(void) { level_c(); }
void __attribute__((noinline)) level_a(void) { level_b(); }

int main(void) {
    puts("03_scan: crashing without FP or CFI — expect scan (HEURISTIC) trust");
    level_a();
    return 0;
}
