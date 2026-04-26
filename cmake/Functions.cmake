function(xk_add_benchmark name)
  cmake_parse_arguments(BENCH "" "" "SOURCES;LIBS" ${ARGN})

  add_executable(${name} ${BENCH_SOURCES})
  target_link_libraries(${name} PRIVATE
    ${BENCH_LIBS}
    GBenchmark::benchmark_main
  )
  # Disable -Werror for benchmark C++ code
  target_compile_options(${name} PRIVATE -Wno-error)
endfunction()
