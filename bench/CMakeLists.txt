# bench/CMakeLists.txt - xKit End-to-End Benchmark Suite

# ── TCP echo server & client (always available since xnet is always built) ──

add_executable(tcp_echo_server tcp_echo_server.cpp)
target_link_libraries(tcp_echo_server PRIVATE xnet xbase)
target_compile_options(tcp_echo_server PRIVATE -Wno-error)

add_executable(tcp_echo_client tcp_echo_client.cpp)
target_link_libraries(tcp_echo_client PRIVATE xbase)
target_compile_options(tcp_echo_client PRIVATE -Wno-error)

# ── HTTP benchmark server (only when xhttp is available) ──

if(Libcurl_FOUND)
  add_executable(http_bench_server http_bench_server.cpp)
  target_link_libraries(http_bench_server PRIVATE xhttp xnet xbase)
  target_compile_options(http_bench_server PRIVATE -Wno-error)

  add_executable(https_bench_server https_bench_server.cpp)
  target_link_libraries(https_bench_server PRIVATE xhttp xnet xbase)
  target_compile_options(https_bench_server PRIVATE -Wno-error)
endif()
