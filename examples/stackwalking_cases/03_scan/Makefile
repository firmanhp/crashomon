CC       = gcc
OBJCOPY  = objcopy

# No -Ox: unwind behaviour is controlled only by the explicit flags below.
#
# -g                          : emit .debug_info so dump_syms produces FUNC/LINE records
# -fomit-frame-pointer        : no rbp chain — forces scan fallback
# -fno-optimize-sibling-calls : emit CALL (not JMP) so return addresses stay on
#                               the stack for the scanner to find
#
# After compilation, objcopy strips .eh_frame, .eh_frame_hdr, and .debug_frame
# so that dump_syms writes NO STACK CFI records.  With no CFI and no frame
# pointer chain, the only option is heuristic stack scanning → trust: scan.
CFLAGS = -g -fomit-frame-pointer \
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
