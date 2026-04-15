# Microbenchmarks — enabled with -DENABLE_BENCHMARKS=ON

set(BENCH_SOURCES
  bench_tombstone_formatter.cpp
  bench_minidump_reader.cpp
)

add_executable(crashomon_bench ${BENCH_SOURCES})

target_link_libraries(crashomon_bench
  PRIVATE
    crashomon_warnings
    crashomon_tombstone
    breakpad_processor
    absl::status
    absl::statusor
    benchmark::benchmark_main
)

target_include_directories(crashomon_bench
  PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(absl_SOURCE_DIR)
  target_include_directories(crashomon_bench SYSTEM PRIVATE "${absl_SOURCE_DIR}")
endif()
if(crashomon_breakpad_SOURCE_DIR)
  target_include_directories(crashomon_bench SYSTEM PRIVATE "${crashomon_breakpad_SOURCE_DIR}/src")
endif()
