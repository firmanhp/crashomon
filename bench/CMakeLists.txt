# Microbenchmarks — enabled with -DENABLE_BENCHMARKS=ON
# Benchmarks are added here as components are implemented.
# Each benchmark file exercises a single module.

set(BENCH_SOURCES
  # Populated as components are built:
  # bench_tombstone_formatter.cpp
  # bench_minidump_reader.cpp
)

if(BENCH_SOURCES)
  add_executable(crashomon_bench ${BENCH_SOURCES})
  target_link_libraries(crashomon_bench
    PRIVATE
      crashomon_warnings
      benchmark::benchmark_main
  )
  target_include_directories(crashomon_bench
    PRIVATE
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()
