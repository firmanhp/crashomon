# Example crasher programs — used for integration testing and fixture generation.
# These programs intentionally crash, so we suppress null-dereference warnings
# that would otherwise be errors under our -Werror policy.

add_executable(example_segfault segfault.c)
# Do not link crashomon_warnings: its -Wnull-dereference would override our -Wno suppression
# (INTERFACE flags are appended after PRIVATE, so last flag wins).
target_compile_options(example_segfault PRIVATE
  -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
  -Wdouble-promotion -Wformat=2 -Wformat-security -fstack-protector-strong
  # Intentional null deref — this IS the crash we want to test.
  -Wno-null-dereference
)
set_target_properties(example_segfault PROPERTIES OUTPUT_NAME crashomon-example-segfault)

add_executable(example_abort abort.c)
target_link_libraries(example_abort PRIVATE crashomon_warnings)
set_target_properties(example_abort PROPERTIES OUTPUT_NAME crashomon-example-abort)

add_executable(example_multithread multithread_crash.c)
target_link_libraries(example_multithread PRIVATE pthread)
# Do not link crashomon_warnings: same -Wnull-dereference override issue as example_segfault.
target_compile_options(example_multithread PRIVATE
  -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion
  -Wdouble-promotion -Wformat=2 -Wformat-security -fstack-protector-strong
  # Intentional null deref in thread — this IS the crash we want to test.
  -Wno-null-dereference
  # usleep is a BSD/misc extension; _DEFAULT_SOURCE enables __USE_MISC which exposes it.
  # _POSIX_C_SOURCE >= 200809L would BLOCK usleep (POSIX 2008 removed it).
  -D_DEFAULT_SOURCE
)
set_target_properties(example_multithread PROPERTIES OUTPUT_NAME crashomon-example-multithread)
