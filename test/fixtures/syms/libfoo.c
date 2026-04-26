/* libfoo.c — minimal shared library fixture for crashomon-syms tests.
 *
 * The VERSION macro is used by the prune tests to produce three binaries
 * with distinct build IDs from the same source file.
 */
#ifndef VERSION
#define VERSION 0
#endif
#define _XSTR(x) #x
#define _STR(x) _XSTR(x)

int foo_add(int a, int b) {
    return a + b;
}

int foo_subtract(int a, int b) {
    return a - b;
}

const char *foo_name(void) {
    return "libfoo";
}

/* Embeds VERSION into .rodata, guaranteeing distinct binary content per build. */
const char *foo_build_tag(void) {
    return "version=" _STR(VERSION);
}
