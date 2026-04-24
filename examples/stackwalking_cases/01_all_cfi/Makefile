CC     = gcc

# No -Ox: unwind behaviour is determined solely by the explicit flags below,
# not by whatever a given compiler enables at a particular optimisation level.
#
# -g                          : emit .debug_info and full prologue .eh_frame records
# -fasynchronous-unwind-tables: write .eh_frame at every instruction so the
#                               stackwalker can unwind at any program point.
#                               This is the ABI default on x86-64 Linux, but
#                               making it explicit ensures the same result on
#                               GCC and Clang regardless of -Ox.
# -fno-omit-frame-pointer     : maintain the rbp chain (belt-and-suspenders;
#                               CFI alone is sufficient but FP gives a second
#                               unwind path if CFI data is absent or corrupt)
# -fno-optimize-sibling-calls : emit CALL (not JMP) at every call site,
#                               keeping all return addresses on the stack
CFLAGS = -g -fasynchronous-unwind-tables \
         -fno-omit-frame-pointer \
         -fno-optimize-sibling-calls \
         -Wall -Wextra

BINARY = crasher

.PHONY: all clean

all: $(BINARY)

$(BINARY): crasher.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINARY)
