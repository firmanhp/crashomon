/*
 * Case 04: mixed CFI / frame-pointer / scan trust in one binary.
 *
 * The call chain crosses three compilation boundaries, each with different flags.
 * As the stackwalker unwinds each frame it uses the best unwind method available
 * for the callee it just recovered.
 *
 * raise(SIGSEGV) is used instead of a null-pointer dereference to prevent
 * GCC from eliminating the call chain via UB/inter-procedural analysis.
 *
 * Call chain (deepest call last):
 *   main  (CFI)
 *     → cfi_relay_outer  (CFI)
 *       → cfi_relay_inner  (CFI)
 *         → fp_relay_outer  (frame pointer, no CFI)
 *           → fp_relay_inner  (frame pointer, no CFI)
 *             → scan_relay  (no FP, no CFI)
 *               → scan_crash → raise(SIGSEGV)
 *
 * Expected trust sequence (bottom to top):
 *   #0  raise / kill    — context        (crash register state inside libc)
 *   #1  scan_crash      — cfi            (recovered via libc .eh_frame)
 *   #2  scan_relay      — scan           (scan_crash: no FP, no CFI in .sym)
 *   #3  fp_relay_inner  — scan           (scan_relay: no FP, no CFI in .sym)
 *   #4  fp_relay_outer  — frame_pointer  (fp_relay_inner: FP, no CFI)
 *   #5  cfi_relay_inner — frame_pointer  (fp_relay_outer: FP, no CFI)
 *   #6  cfi_relay_outer — cfi            (cfi_relay_inner has .eh_frame)
 *   #7  main            — cfi            (cfi_relay_outer has .eh_frame)
 *
 * Key insight: the trust of frame N is determined by the callee (frame N-1),
 * NOT by how frame N itself was compiled.
 */

#include <stdio.h>
#include "mixed.h"

int main(void) {
    puts("04_mixed: crashing across compilation boundaries"
         " — expect context/scan/frame_pointer/cfi");
    cfi_relay_outer();
    return 0;
}
