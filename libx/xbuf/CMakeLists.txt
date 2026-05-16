file(GLOB_RECURSE XBUF_HEADERS "*.h")
file(GLOB_RECURSE XBUF_SOURCES "*.c")
add_library(xbuf ${MOO_LIB_TYPE} ${XBUF_SOURCES} ${XBUF_HEADERS})

target_link_libraries(xbuf PUBLIC xbase)

message(STATUS "xbuf: buffer module enabled (xBuffer + xRingBuffer + xIOBuffer)")

option(XBUF_BUILD_TESTS      "Build xbuf tests"      ${MOO_BUILD_TESTS})
option(XBUF_BUILD_BENCHMARKS "Build xbuf benchmarks" ${MOO_BUILD_BENCHMARKS})

if(XBUF_BUILD_TESTS)
  file(GLOB_RECURSE XBUF_TEST_SOURCES "*_test.cpp")
  if(XBUF_TEST_SOURCES)
    add_executable(xbuf_test ${XBUF_TEST_SOURCES})
    target_link_libraries(xbuf_test PRIVATE xbuf GTest::gtest_main)
    add_test(NAME xbuf_test COMMAND xbuf_test)
  endif()
endif()

if(XBUF_BUILD_BENCHMARKS)
  file(GLOB XBUF_BENCH_SOURCES "*_bench.cpp")
  foreach(src ${XBUF_BENCH_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    xk_add_benchmark(${name}
      SOURCES ${src}
      LIBS    xbuf
    )
  endforeach()
endif()
