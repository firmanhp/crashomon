#include "crashomon.h"

#include <sentry.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Defaults ──────────────────────────────────────────────────────────────── */

static const char kDefaultDbPath[] = "/var/crashomon";
static const char kDefaultHandlerPath[] = "/usr/libexec/crashomon/crashpad_handler";
static const char kDummyDsn[] = "https://00000000000000000000000000000000@localhost/0";

/* ── Internal init ─────────────────────────────────────────────────────────── */

static int do_init(const char *db_path, const char *handler_path) {
  sentry_options_t *options = sentry_options_new();
  if (!options) return -1;

  sentry_options_set_dsn(options, kDummyDsn);
  sentry_options_set_database_path(options, db_path);
  sentry_options_set_handler_path(options, handler_path);

  /* Disable all uploads — local only. */
  sentry_options_set_auto_session_tracking(options, 0);
  sentry_options_set_traces_sample_rate(options, 0.0);

  return sentry_init(options);
}

/* ── LD_PRELOAD constructor / destructor ───────────────────────────────────── */

__attribute__((constructor)) static void crashomon_auto_init(void) {
  const char *db_path = getenv("CRASHOMON_DB_PATH");
  if (!db_path) db_path = kDefaultDbPath;

  const char *handler_path = getenv("CRASHOMON_HANDLER_PATH");
  if (!handler_path) handler_path = kDefaultHandlerPath;

  do_init(db_path, handler_path);
}

__attribute__((destructor)) static void crashomon_auto_shutdown(void) { sentry_close(); }

/* ── Public API ────────────────────────────────────────────────────────────── */

int crashomon_init(const CrashomonConfig *config) {
  const char *db_path = kDefaultDbPath;
  const char *handler_path = kDefaultHandlerPath;

  if (config) {
    if (config->db_path) db_path = config->db_path;
    if (config->handler_path) handler_path = config->handler_path;
  }

  return do_init(db_path, handler_path);
}

void crashomon_shutdown(void) { sentry_close(); }

void crashomon_set_tag(const char *key, const char *value) {
  if (key && value) sentry_set_tag(key, value);
}

void crashomon_add_breadcrumb(const char *message) {
  if (!message) return;
  sentry_value_t crumb = sentry_value_new_breadcrumb("default", message);
  sentry_add_breadcrumb(crumb);
}

void crashomon_set_abort_message(const char *message) {
  if (message) sentry_set_tag("abort_message", message);
}
