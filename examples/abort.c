// Example: triggers SIGABRT via assert failure.
// Used for integration testing and fixture .dmp generation.

#include <assert.h>
#include <stdio.h>

static void do_crash(void) { assert(0 && "intentional assertion failure"); }

int main(void) {
  printf("crashomon example: triggering SIGABRT...\n");
  do_crash();
  return 0;
}
