/*
 * Case 05: portable explicit flags
 *
 * The same source file is compiled three times with different flag sets to
 * produce three binaries:
 *
 *   crasher_cfi   — -fasynchronous-unwind-tables -fno-omit-frame-pointer
 *   crasher_fp    — -fno-omit-frame-pointer  + objcopy strip CFI
 *   crasher_scan  — -fomit-frame-pointer     + objcopy strip CFI
 *
 * Every flag that affects unwind behaviour is spelled out explicitly.  No
 * -Ox level is used, so the result is the same on GCC and Clang regardless
 * of what each compiler enables at -O1 or -O2.
 *
 * Expected trust sequences:
 *
 *   crasher_cfi:
 *     #0  raise/kill   — context
 *     #1  crash_here   — cfi
 *     #2  level_d      — cfi
 *     ...
 *     #6  main         — cfi
 *
 *   crasher_fp:
 *     #0  raise/kill   — context
 *     #1  crash_here   — cfi       (libc has .eh_frame; our .sym does not)
 *     #2  level_d      — frame_pointer
 *     ...
 *     #6  main         — frame_pointer
 *
 *   crasher_scan:
 *     #0  raise/kill   — context
 *     #1  crash_here   — cfi       (libc has .eh_frame; our .sym does not)
 *     #2  level_d      — scan
 *     ...
 *     #6  main         — scan
 */

#include <signal.h>
#include <stdio.h>

void __attribute__((noinline)) crash_here(void) { raise(SIGSEGV); }
void __attribute__((noinline)) level_d(void) { crash_here(); }
void __attribute__((noinline)) level_c(void) { level_d(); }
void __attribute__((noinline)) level_b(void) { level_c(); }
void __attribute__((noinline)) level_a(void) { level_b(); }

int main(void) {
    puts("05_portable_explicit: same source, three explicit flag sets");
    level_a();
    return 0;
}
