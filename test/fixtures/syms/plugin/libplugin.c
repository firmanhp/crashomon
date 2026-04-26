/* libplugin.c — plugin-subdir fixture for sysroot recursive-scan tests.
 *
 * Placed under plugin/ to verify that crashomon-syms --sysroot walks into
 * subdirectories of the scanned lib paths (e.g. usr/lib/plugin/).
 */

int plugin_init(int flags) {
    return flags & 1;
}

void plugin_cleanup(void) {
    /* nothing */
}

const char *plugin_name(void) {
    return "libplugin";
}
