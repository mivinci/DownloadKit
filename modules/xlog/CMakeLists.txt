file(GLOB_RECURSE XLOG_HEADERS "*.h")
file(GLOB_RECURSE XLOG_SOURCES "*.c")
add_library(xlog ${XK_LIB_TYPE} ${XLOG_SOURCES} ${XLOG_HEADERS})

target_link_libraries(xlog PUBLIC xbase)

message(STATUS "xlog: async logging module enabled")

if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XLOG_TEST_SOURCES "*_test.cpp")
  if(XLOG_TEST_SOURCES)
    add_executable(xlog_test ${XLOG_TEST_SOURCES})
    target_link_libraries(xlog_test PRIVATE xlog GTest::gtest_main)
    add_test(NAME xlog_test COMMAND xlog_test)
  endif()
endif()
