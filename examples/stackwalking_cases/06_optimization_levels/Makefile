# CC defaults to gcc; override with: make CC=clang
CC ?= gcc

# No explicit unwind-behaviour flags — just what a normal developer build uses.
# On x86-64 Linux, -fasynchronous-unwind-tables is the ABI default at every -Ox,
# so .eh_frame (and therefore STACK CFI) is always present regardless of level.
#
# What DOES change with -Ox:
#   -O0           : frame pointer kept (no -fomit-frame-pointer)
#   -O1 and above : -fomit-frame-pointer enabled by default in GCC/Clang
#
# With a .sym file all four binaries produce cfi trust throughout.
# Without a .sym file, crasher_O0 falls back to frame_pointer while
# crasher_O1/O2/O3 fall back to scan (no rbp chain, heuristic only).

BINARIES = crasher_O0 crasher_O1 crasher_O2 crasher_O3

.PHONY: all clean

all: $(BINARIES)

crasher_O0: crasher.c
	$(CC) -O0 -g -Wall -Wextra -fno-optimize-sibling-calls -o $@ $<

crasher_O1: crasher.c
	$(CC) -O1 -g -Wall -Wextra -fno-optimize-sibling-calls -o $@ $<

crasher_O2: crasher.c
	$(CC) -O2 -g -Wall -Wextra -fno-optimize-sibling-calls -o $@ $<

crasher_O3: crasher.c
	$(CC) -O3 -g -Wall -Wextra -fno-optimize-sibling-calls -o $@ $<

clean:
	rm -f $(BINARIES)
