/*
 * Case 01: all CFI
 *
 * Compiled with -g and the default -fasynchronous-unwind-tables so every
 * function has .eh_frame coverage.  When dump_syms extracts symbols and
 * minidump-stackwalk processes the dump with that .sym file, every frame
 * above the crash site is recovered via Call Frame Information.
 *
 * raise(SIGSEGV) is used instead of a null-pointer dereference because GCC
 * inter-procedural analysis treats *(int*)0 = 1 as UB and eliminates the
 * entire call chain at -O1 or higher.  raise() is an external function call
 * with an observable side effect, so the chain is preserved.
 *
 * Expected trust sequence (bottom to top):
 *   #0  raise / kill   — context        (crash register state inside libc)
 *   #1  crash_here     — cfi            (recovered via libc .eh_frame)
 *   #2  level_d        — cfi
 *   #3  level_c        — cfi
 *   #4  level_b        — cfi
 *   #5  level_a        — cfi
 *   #6  main           — cfi
 */

#include <signal.h>
#include <stdio.h>

void __attribute__((noinline)) crash_here(void) { raise(SIGSEGV); }
void __attribute__((noinline)) level_d(void) { crash_here(); }
void __attribute__((noinline)) level_c(void) { level_d(); }
void __attribute__((noinline)) level_b(void) { level_c(); }
void __attribute__((noinline)) level_a(void) { level_b(); }

int main(void) {
    puts("01_all_cfi: crashing with full CFI coverage — expect all cfi trust");
    level_a();
    return 0;
}
