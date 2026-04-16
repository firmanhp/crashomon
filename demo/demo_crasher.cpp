// demo_crasher.cpp — demonstration crasher for the crashomon demo
//
// Simulates a multi-stage data-processing pipeline running across three named
// threads.  The crash happens in the "processor" thread with a deep call stack;
// the "logger" and "monitor" threads are live in the background and appear in
// the minidump with their own distinct stacks.
//
// Thread layout visible in the crash report:
//   main       — stuck in pthread_join waiting for the processor
//   logger     — perpetual sleep loop (simulates background log flushing)
//   monitor    — half-second health-check loop
//   processor  — pipeline runner; crashes on record 3
//                  processor_thread_fn → run_pipeline → validate_and_process
//                    → compute_score → store_result  ← SIGSEGV here
//
// Design note: the pipeline functions are [[gnu::noinline]] AND each returns
// a value used by its caller, preventing the compiler from replacing the
// downstream call with a tail-call (jmp).  Without this, -O3 tail-call
// optimisation collapses compute_score and validate_and_process off the stack.
//
// Build and run via:  demo/run_demo.sh

#include "crashomon.h"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

namespace {

// A record flowing through the pipeline.
// result_out points to the caller's output buffer — null is the bug.
struct Record {
    const char* key;
    const char* value;
    int*        result_out;  // intentionally null for the third record
};

// Score multiplier — arbitrary constant for the demo computation.
constexpr int kScoreMultiplier = 7;

// ── Pipeline stages ────────────────────────────────────────────────────────────
// All four stages are [[gnu::noinline]] and return a value used by their
// caller.  Returning a used value prevents tail-call optimisation: the caller
// cannot replace `call + ret` with `jmp` because it has arithmetic left to do
// on the return value after the callee returns.
//
//   processor_thread_fn → run_pipeline → validate_and_process
//     → compute_score → store_result  ← SIGSEGV on record 3

// Stage 4 — write score into the output buffer and return it.
// Crashes with SIGSEGV when destination is null.
[[gnu::noinline]] int store_result(int score, int* destination) {
    *destination = score;  // NULL dereference when destination == nullptr
    return score;
}

// Stage 3 — compute score, store it, and return score + 1 (the +1 is the
// work done *after* store_result that prevents store_result becoming a tail call).
[[gnu::noinline]] int compute_score(const Record* rec) {
    int score = static_cast<int>(::strlen(rec->key) + ::strlen(rec->value)) * kScoreMultiplier;
    int stored = store_result(score, rec->result_out);
    return stored + 1;  // post-call arithmetic ensures store_result is not a tail call
}

// Stage 2 — validate, dispatch to compute_score, return score * 2.
// The *2 is work done after compute_score that prevents compute_score becoming
// a tail call.
[[gnu::noinline]] int validate_and_process(const Record* rec) {
    if (rec->key == nullptr || rec->value == nullptr) {
        std::fprintf(stderr, "invalid record: missing key or value\n");
        return -1;
    }
    int result = compute_score(rec);
    return result * 2;  // post-call arithmetic ensures compute_score is not a tail call
}

// Stage 1 — iterate over the record array and dispatch each record.
[[gnu::noinline]] void run_pipeline(const Record* records, int count) {
    for (int i = 0; i < count; ++i) {
        std::printf("  [processor] processing record %d: key=%s\n",
                    i + 1, records[i].key);
        std::fflush(stdout);
        validate_and_process(&records[i]);
    }
}

// ── Background threads ─────────────────────────────────────────────────────────
// These threads stay alive so they appear in the minidump with recognisable
// stacks, demonstrating that Crashpad captures all threads, not just the one
// that crashed.  Thread names are set via pthread_setname_np and are extracted
// from the minidump's ThreadNamesStream by crashomon-analyze.

// "logger" — simulates a background log-flushing loop.
void* logger_thread_fn(void* /*arg*/) {
    pthread_setname_np(pthread_self(), "logger");
    for (;;) {
        sleep(1);  // will appear in the minidump blocked here
    }
}

// "monitor" — simulates a periodic health-check loop.
void* monitor_thread_fn(void* /*arg*/) {
    pthread_setname_np(pthread_self(), "monitor");
    for (;;) {
        usleep(500'000);  // 500 ms, will appear in the minidump blocked here
    }
}

// "processor" — runs the pipeline; crashes on record 3.
void* processor_thread_fn(void* /*arg*/) {
    pthread_setname_np(pthread_self(), "processor");

    // Brief pause so logger and monitor enter their sleep loops before the crash.
    // Their stacks will clearly show the blocking syscall in the minidump.
    usleep(20'000);  // 20 ms

    int r1 = 0;
    int r2 = 0;
    const Record records[] = {
        {"alpha", "hello-world",    &r1},
        {"beta",  "crash-demo",     &r2},
        {"gamma", "sentinel-value", nullptr},  // null result_out → crash
    };

    run_pipeline(records, static_cast<int>(sizeof(records) / sizeof(records[0])));
    return nullptr;
}

}  // namespace

int main() {
    // libcrashomon.so has a __attribute__((constructor)) that auto-initializes
    // when the library is loaded — whether via LD_PRELOAD or explicit linking.
    // The constructor reads CRASHOMON_SOCKET_PATH (set by run_demo.sh) from the
    // environment, so no explicit crashomon_init() call is needed here.
    //
    // crashomon_set_tag() / crashomon_set_abort_message() are safe to call after
    // the constructor has run; they populate the CrashpadInfo annotation dict.

    crashomon_set_tag("component", "demo-pipeline");
    crashomon_set_tag("version",   "1.0.0-demo");
    crashomon_set_tag("threads",   "3");

    crashomon_set_abort_message(
        "null result_out in pipeline stage store_result: "
        "record 3 missing output buffer");

    std::printf("crashomon demo: spawning logger, monitor, and processor threads\n");
    std::fflush(stdout);

    pthread_t t_logger;
    pthread_t t_monitor;
    pthread_t t_processor;
    pthread_create(&t_logger,    nullptr, logger_thread_fn,    nullptr);
    pthread_create(&t_monitor,   nullptr, monitor_thread_fn,   nullptr);
    pthread_create(&t_processor, nullptr, processor_thread_fn, nullptr);

    // Block on the processor.  It will crash, so this pthread_join will not
    // return — main stays visible in the minidump waiting here.
    pthread_join(t_processor, nullptr);
    return 0;
}
