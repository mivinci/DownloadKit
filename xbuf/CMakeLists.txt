file(GLOB_RECURSE XBUF_HEADERS "*.h")
file(GLOB_RECURSE XBUF_SOURCES "*.c")
add_library(xbuf SHARED ${XBUF_SOURCES} ${XBUF_HEADERS})

target_link_libraries(xbuf PUBLIC xbase)

message(STATUS "xbuf: buffer module enabled (xBuffer + xRingBuffer + xIOBuffer)")

if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XBUF_TEST_SOURCES "*_test.cpp")
  if(XBUF_TEST_SOURCES)
    add_executable(xbuf_test ${XBUF_TEST_SOURCES})
    target_link_libraries(xbuf_test PRIVATE xbuf GTest::gtest GTest::gtest_main)
    add_test(NAME xbuf_test COMMAND xbuf_test)
  endif()
endif()
