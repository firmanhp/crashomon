/*
 * Case 02: frame pointer only
 *
 * Compiled with -g -fno-omit-frame-pointer.  After compilation the Makefile
 * strips .eh_frame, .eh_frame_hdr, and .debug_frame with objcopy, so dump_syms
 * writes FUNC/LINE records (from .debug_info) but no STACK CFI records.  The
 * stackwalker finds no CFI and falls back to the rbp chain.
 *
 * raise(SIGSEGV) is used instead of a null-pointer dereference — see case 01
 * for the reason.
 *
 * Expected trust sequence (bottom to top):
 *   #0  raise / kill   — context        (crash register state inside libc)
 *   #1  crash_here     — cfi            (libc has full .eh_frame)
 *   #2  level_d        — frame_pointer  (crash_here: rbp chain, no CFI in .sym)
 *   #3  level_c        — frame_pointer
 *   #4  level_b        — frame_pointer
 *   #5  level_a        — frame_pointer
 *   #6  main           — frame_pointer
 */

#include <signal.h>
#include <stdio.h>

void __attribute__((noinline)) crash_here(void) { raise(SIGSEGV); }
void __attribute__((noinline)) level_d(void) { crash_here(); }
void __attribute__((noinline)) level_c(void) { level_d(); }
void __attribute__((noinline)) level_b(void) { level_c(); }
void __attribute__((noinline)) level_a(void) { level_b(); }

int main(void) {
    puts("02_frame_pointer: crashing without .eh_frame — expect frame_pointer trust");
    level_a();
    return 0;
}
