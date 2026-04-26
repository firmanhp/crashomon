/* libbar.c — second shared library fixture for crashomon-syms tests. */

int bar_multiply(int x, int y) {
    return x * y;
}

int bar_square(int x) {
    return x * x;
}

const char *bar_name(void) {
    return "libbar";
}
