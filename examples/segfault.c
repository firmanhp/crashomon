// Example: triggers SIGSEGV by dereferencing a null pointer.
// Used for integration testing and fixture .dmp generation.

#include <stddef.h>
#include <stdio.h>

static void do_crash(void) {
  volatile int* null_ptr = NULL;
  *null_ptr = 42;  // SIGSEGV
}

int main(void) {
  printf("crashomon example: triggering SIGSEGV...\n");
  do_crash();
  return 0;
}
