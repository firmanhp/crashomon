// Example: multi-threaded program where a worker thread triggers SIGSEGV.
// Used to verify that crashpad captures all thread stacks, not just the crashing one.

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

static void* worker_crash(void* arg) {
  (void)arg;
  // Spin briefly so the main thread is clearly in a different state.
  usleep(50000);  // 50ms
  volatile int* null_ptr = NULL;
  *null_ptr = 42;  // SIGSEGV from worker thread
  return NULL;
}

static void* worker_idle(void* arg) {
  (void)arg;
  // This thread stays alive (sleeping) to appear in the crash dump.
  sleep(60);
  return NULL;
}

int main(void) {
  printf("crashomon example: multi-thread SIGSEGV...\n");

  pthread_t idle_thread;
  pthread_create(&idle_thread, NULL, worker_idle, NULL);

  pthread_t crash_thread;
  pthread_create(&crash_thread, NULL, worker_crash, NULL);

  pthread_join(crash_thread, NULL);
  pthread_join(idle_thread, NULL);
  return 0;
}
