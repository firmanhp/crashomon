/*
 * Compiled with -fomit-frame-pointer.  The Makefile strips .eh_frame,
 * .eh_frame_hdr, and .debug_frame from the object file via objcopy.
 * Neither CFI nor rbp chain.  -fno-optimize-sibling-calls keeps all CALL
 * instructions so return addresses stay on the stack for scanning.
 * Unwinding through these frames produces trust: scan (heuristic).
 *
 * raise(SIGSEGV) is used instead of a null-pointer dereference to prevent
 * GCC from eliminating the call chain via UB/inter-procedural analysis.
 */

#include <signal.h>
#include "mixed.h"

void __attribute__((noinline)) scan_crash(void) { raise(SIGSEGV); }
void __attribute__((noinline)) scan_relay(void) { scan_crash(); }
