/*
 * Compiled with -fno-omit-frame-pointer.  The Makefile strips .eh_frame,
 * .eh_frame_hdr, and .debug_frame from the object file via objcopy so that
 * dump_syms writes no STACK CFI for these functions.  The rbp chain is intact.
 * Unwinding through these frames produces trust: frame_pointer.
 */

#include "mixed.h"

void __attribute__((noinline)) fp_relay_outer(void) { fp_relay_inner(); }
void __attribute__((noinline)) fp_relay_inner(void) { scan_relay(); }
