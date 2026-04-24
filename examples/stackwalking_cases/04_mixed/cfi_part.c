/*
 * Compiled with -fasynchronous-unwind-tables (default on x86-64) and
 * -fno-omit-frame-pointer.  dump_syms extracts STACK CFI records from .eh_frame
 * for both functions.  Unwinding through these frames produces trust: cfi.
 */

#include "mixed.h"

void __attribute__((noinline)) cfi_relay_outer(void) { cfi_relay_inner(); }
void __attribute__((noinline)) cfi_relay_inner(void) { fp_relay_outer(); }
