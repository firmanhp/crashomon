CC      = gcc
OBJCOPY = objcopy

# No -Ox: unwind behaviour is controlled only by the explicit per-TU flags below.
# All TUs share these base flags:
COMMON  = -g -fno-optimize-sibling-calls -Wall -Wextra

# Full CFI: explicit -fasynchronous-unwind-tables + frame pointer.
# dump_syms writes STACK CFI → trust: cfi
CFLAGS_CFI  = $(COMMON) -fasynchronous-unwind-tables -fno-omit-frame-pointer

# Frame pointer, no CFI: rbp chain intact; .eh_frame/.debug_frame stripped by
# objcopy after compilation so dump_syms writes NO STACK CFI → trust: frame_pointer
CFLAGS_FP   = $(COMMON) -fno-omit-frame-pointer

# No FP, no CFI: .eh_frame/.debug_frame stripped; no rbp chain → trust: scan
CFLAGS_SCAN = $(COMMON) -fomit-frame-pointer

BINARY  = crasher
OBJS    = main.o cfi_part.o fp_part.o scan_part.o

.PHONY: all clean

all: $(BINARY)

$(BINARY): $(OBJS)
	$(CC) -o $@ $(OBJS)

main.o: main.c mixed.h
	$(CC) $(CFLAGS_CFI) -c -o $@ $<

cfi_part.o: cfi_part.c mixed.h
	$(CC) $(CFLAGS_CFI) -c -o $@ $<

# Compile fp_part and scan_part into a temp object, then strip the CFI sections
# so that dump_syms produces no STACK CFI for these functions.
fp_part.o: fp_part.c mixed.h
	$(CC) $(CFLAGS_FP) -c -o $@.tmp $<
	$(OBJCOPY) --remove-section=.eh_frame \
	           --remove-section=.eh_frame_hdr \
	           --remove-section=.debug_frame \
	           $@.tmp $@
	rm -f $@.tmp

scan_part.o: scan_part.c mixed.h
	$(CC) $(CFLAGS_SCAN) -c -o $@.tmp $<
	$(OBJCOPY) --remove-section=.eh_frame \
	           --remove-section=.eh_frame_hdr \
	           --remove-section=.debug_frame \
	           $@.tmp $@
	rm -f $@.tmp

clean:
	rm -f $(BINARY) $(OBJS) fp_part.o.tmp scan_part.o.tmp
