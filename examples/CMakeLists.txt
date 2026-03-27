# Example crasher programs — used for integration testing and fixture generation.

add_executable(example_segfault segfault.c)
target_link_libraries(example_segfault PRIVATE crashomon_warnings)
set_target_properties(example_segfault PROPERTIES OUTPUT_NAME crashomon-example-segfault)

add_executable(example_abort abort.c)
target_link_libraries(example_abort PRIVATE crashomon_warnings)
set_target_properties(example_abort PROPERTIES OUTPUT_NAME crashomon-example-abort)

add_executable(example_multithread multithread_crash.c)
target_link_libraries(example_multithread PRIVATE crashomon_warnings pthread)
set_target_properties(example_multithread PROPERTIES OUTPUT_NAME crashomon-example-multithread)
