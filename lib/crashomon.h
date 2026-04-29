#ifndef CRASHOMON_CRASHOMON_H_
#define CRASHOMON_CRASHOMON_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * crashomon — crash monitoring client library (Crashpad backend)
 *
 * Usage (LD_PRELOAD):
 *   LD_PRELOAD=/usr/lib/libcrashomon.so CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock
 * ./my_program
 *
 * Usage (explicit linking):
 *   Link with -lcrashomon. The library constructor initializes crash monitoring
 *   automatically — no explicit init call is needed.
 *
 * Environment variables:
 *   CRASHOMON_DB_PATH     Directory where minidumps are written (watcherd side).
 *                         Default: /var/crashomon
 *   CRASHOMON_SOCKET_PATH Path to the crashomon-watcherd Unix domain socket.
 *                         Default: /run/crashomon/handler.sock
 */

// The functions below use snake_case C naming — this is a C public API.
// GlobalFunctionCase: lower_case in .clang-tidy covers extern "C" functions.

/**
 * Attach a key-value tag to all crash reports from this process.
 * Tags are stored in Crashpad's SimpleStringDictionary and included in the
 * minidump. Keys and values are truncated to 255 characters; up to 64 entries
 * are supported. Setting the same key twice overwrites the previous value.
 * Example: crashomon_set_tag("version", "1.2.3")
 */
void crashomon_set_tag(const char *key, const char *value);

/**
 * Set an abort message that will appear in the crash report.
 * Stored as the "abort_message" annotation, which Crashpad tooling recognises
 * by convention (matching Android's android_set_abort_message() behaviour).
 * Call this before abort() to attach context, e.g. an assertion detail.
 * Example: crashomon_set_abort_message("invariant violated: count >= 0")
 */
void crashomon_set_abort_message(const char *message);

#ifdef __cplusplus
}
#endif

#endif /* CRASHOMON_CRASHOMON_H_ */
