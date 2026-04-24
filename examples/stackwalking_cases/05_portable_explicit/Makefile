# CC defaults to gcc; override with: make CC=clang
CC      ?= gcc
OBJCOPY ?= objcopy

# ── Portable flag sets ────────────────────────────────────────────────────────
#
# These flag sets are compiler-agnostic: they produce the same unwind behaviour
# on GCC and Clang regardless of optimization level defaults.  Every flag that
# affects unwinding is spelled out; nothing is left to -Ox defaults.
#
# CFLAGS_CFI: full DWARF CFI in .eh_frame at every instruction.
#   -fasynchronous-unwind-tables  generate .eh_frame for ALL program points
#                                  (not just call sites).  ABI default on
#                                  x86-64 Linux but explicit here.
#   -fno-omit-frame-pointer       keep the rbp chain as a second unwind path
#   -fno-optimize-sibling-calls   force CALL (not JMP) at every call site
#
# CFLAGS_FP: frame pointer but no DWARF CFI in the .sym file.
#   -fno-omit-frame-pointer       keep rbp chain
#   -fno-optimize-sibling-calls   keep return addresses on the stack
#   (objcopy strips .eh_frame/.debug_frame so dump_syms sees no STACK CFI)
#
# CFLAGS_SCAN: neither frame pointer nor CFI.
#   -fomit-frame-pointer          no rbp chain
#   -fno-optimize-sibling-calls   keep return addresses on the stack for scanning
#   (objcopy strips .eh_frame/.debug_frame)

BASE    = -g -fno-optimize-sibling-calls -Wall -Wextra

CFLAGS_CFI  = $(BASE) -fasynchronous-unwind-tables -fno-omit-frame-pointer
CFLAGS_FP   = $(BASE) -fno-omit-frame-pointer
CFLAGS_SCAN = $(BASE) -fomit-frame-pointer

BINARIES = crasher_cfi crasher_fp crasher_scan

.PHONY: all clean

all: $(BINARIES)

crasher_cfi: crasher.c
	$(CC) $(CFLAGS_CFI) -o $@ $<

crasher_fp: crasher.c
	$(CC) $(CFLAGS_FP) -o $@.tmp $<
	$(OBJCOPY) --remove-section=.eh_frame \
	           --remove-section=.eh_frame_hdr \
	           --remove-section=.debug_frame \
	           $@.tmp $@
	rm -f $@.tmp

crasher_scan: crasher.c
	$(CC) $(CFLAGS_SCAN) -o $@.tmp $<
	$(OBJCOPY) --remove-section=.eh_frame \
	           --remove-section=.eh_frame_hdr \
	           --remove-section=.debug_frame \
	           $@.tmp $@
	rm -f $@.tmp

clean:
	rm -f $(BINARIES) crasher_fp.tmp crasher_scan.tmp
