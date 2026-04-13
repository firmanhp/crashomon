/* Minimal curl stub header — part of crashomon's build system.
 * Provides only the types and constants needed to compile
 * Crashpad's http_transport_libcurl.cc. That file uses dlopen to load
 * libcurl at runtime; this stub is never executed with url="" in StartHandler(). */
#pragma once
#include <sys/types.h>
typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
typedef long curl_off_t;
struct curl_slist {
  char *data;
  struct curl_slist *next;
};
/* Global init flags */
#define CURL_GLOBAL_DEFAULT 3L
/* CURLcode values */
#define CURLE_OK 0
/* CURLoption values (numeric values from libcurl 7.x) */
#define CURLOPT_URL 10002
#define CURLOPT_USERAGENT 10018
#define CURLOPT_ACCEPT_ENCODING 10102
#define CURLOPT_CAINFO 10065
#define CURLOPT_TIMEOUT_MS 155
#define CURLOPT_POST 47
#define CURLOPT_POSTFIELDSIZE_LARGE 30115
#define CURLOPT_CUSTOMREQUEST 10036
#define CURLOPT_HTTPHEADER 10023
#define CURLOPT_READFUNCTION 20012
#define CURLOPT_READDATA 10009
#define CURLOPT_WRITEFUNCTION 20011
#define CURLOPT_WRITEDATA 10001
#define CURLOPT_PROXY 10004
/* CURLINFO values */
#define CURLINFO_RESPONSE_CODE 0x200002
/* Read callback abort sentinel */
#define CURL_READFUNC_ABORT 0x10000000
/* Function declarations (loaded via dlsym at runtime, never called here) */
static inline CURL *curl_easy_init(void) { return 0; }
static inline void curl_easy_cleanup(CURL *c) { (void)c; }
static inline CURLcode curl_easy_perform(CURL *c) {
  (void)c;
  return CURLE_OK;
}
static inline CURLcode curl_easy_setopt(CURL *c, CURLoption o, ...) {
  (void)c;
  (void)o;
  return CURLE_OK;
}
static inline CURLcode curl_easy_getinfo(CURL *c, CURLINFO i, ...) {
  (void)c;
  (void)i;
  return CURLE_OK;
}
static inline const char *curl_easy_strerror(CURLcode e) {
  (void)e;
  return "";
}
static inline CURLcode curl_global_init(long flags) {
  (void)flags;
  return CURLE_OK;
}
static inline struct curl_slist *curl_slist_append(struct curl_slist *s,
                                                   const char *d) {
  (void)s;
  (void)d;
  return 0;
}
static inline void curl_slist_free_all(struct curl_slist *s) { (void)s; }
static inline char *curl_version(void) { return (char *)"stub/0"; }
