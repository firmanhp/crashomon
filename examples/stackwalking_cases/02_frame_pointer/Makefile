CC       = gcc
OBJCOPY  = objcopy

# No -Ox: unwind behaviour is controlled only by the explicit flags below.
#
# -g                          : emit .debug_info so dump_syms produces FUNC/LINE records
# -fno-omit-frame-pointer     : maintain the rbp chain for frame-pointer walking
# -fno-optimize-sibling-calls : emit CALL (not JMP) at every call site
#
# After compilation, objcopy strips .eh_frame, .eh_frame_hdr, and .debug_frame
# so that dump_syms writes NO STACK CFI records into the .sym file.  The
# stackwalker then finds no CFI and falls back to the rbp chain → trust: frame_pointer.
#
# Note: -fasynchronous-unwind-tables is deliberately omitted.  The objcopy
# strip removes any .eh_frame emitted by the compiler regardless of whether
# async or synchronous unwind tables were requested.
CFLAGS = -g -fno-omit-frame-pointer \
         -fno-optimize-sibling-calls \
         -Wall -Wextra

BINARY = crasher

.PHONY: all clean

all: $(BINARY)

$(BINARY): crasher.c
	$(CC) $(CFLAGS) -o $@.tmp $<
	$(OBJCOPY) --remove-section=.eh_frame \
	           --remove-section=.eh_frame_hdr \
	           --remove-section=.debug_frame \
	           $@.tmp $@
	rm -f $@.tmp

clean:
	rm -f $(BINARY) $(BINARY).tmp
