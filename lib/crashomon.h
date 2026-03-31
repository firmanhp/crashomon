#ifndef CRASHOMON_CRASHOMON_H_
#define CRASHOMON_CRASHOMON_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * crashomon — crash monitoring client library (Crashpad backend)
 *
 * Usage (LD_PRELOAD):
 *   LD_PRELOAD=/usr/lib/libcrashomon.so CRASHOMON_SOCKET_PATH=/run/crashomon/handler.sock ./my_program
 *
 * Usage (explicit linking):
 *   Link with -lcrashomon and call crashomon_init() at startup.
 *
 * Environment variables:
 *   CRASHOMON_DB_PATH     Directory where minidumps are written (watcherd side).
 *                         Default: /var/crashomon
 *   CRASHOMON_SOCKET_PATH Path to the crashomon-watcherd Unix domain socket.
 *                         Default: /run/crashomon/handler.sock
 */

/**
 * Configuration for explicit init (not needed for LD_PRELOAD).
 * Zero-initialize to use defaults.
 */
// NOLINTNEXTLINE(modernize-use-using) — typedef struct is idiomatic C; consumed by C code.
typedef struct CrashomonConfig {
  const char *db_path;      /* Minidump database path  (NULL = use env/default) */
  const char *socket_path;  /* crashomon-watcherd socket path (NULL = use env/default) */
} CrashomonConfig;

// The functions below use snake_case C naming intentionally — this is a C public API.
// NOLINTBEGIN(readability-identifier-naming)

/**
 * Initialize crash monitoring with explicit config.
 * Not needed when using LD_PRELOAD — the constructor handles init automatically.
 * Returns 0 on success, non-zero on failure.
 */
int crashomon_init(const CrashomonConfig *config);

/**
 * Shut down crash monitoring.
 * Not needed when using LD_PRELOAD — the destructor handles shutdown automatically.
 */
void crashomon_shutdown(void);

/**
 * Attach a key-value tag to all crash reports from this process.
 * Example: crashomon_set_tag("version", "1.2.3")
 *
 * NOLINTNEXTLINE(bugprone-easily-swappable-parameters) — key/value is a standard C API pattern.
 */
void crashomon_set_tag(const char *key, const char *value);

/**
 * Add a breadcrumb (timestamped log entry) to the crash context.
 * Breadcrumbs appear in crash reports to show what the process was doing.
 */
void crashomon_add_breadcrumb(const char *message);

/**
 * Set an abort message that will appear in the crash report.
 * Call this before abort() to attach context (e.g., assertion details).
 * Example: crashomon_set_abort_message("invariant violated: count >= 0")
 */
void crashomon_set_abort_message(const char *message);

// NOLINTEND(readability-identifier-naming)

#ifdef __cplusplus
}
#endif

#endif /* CRASHOMON_CRASHOMON_H_ */
