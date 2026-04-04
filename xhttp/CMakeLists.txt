file(GLOB_RECURSE XHTTP_HEADERS "*.h")
file(GLOB_RECURSE XHTTP_SOURCES "*.c")
add_library(xhttp SHARED ${XHTTP_SOURCES} ${XHTTP_HEADERS})

target_compile_definitions(xhttp PUBLIC XK_HAS_CURL)
target_link_libraries(xhttp PUBLIC xbase xbuf PRIVATE Libcurl::Libcurl)

message(STATUS "xhttp: libcurl integration enabled")

if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XHTTP_TEST_SOURCES "*_test.cpp")
  if(XHTTP_TEST_SOURCES)
    add_executable(xhttp_test ${XHTTP_TEST_SOURCES})
    target_link_libraries(xhttp_test PRIVATE xhttp GTest::gtest GTest::gtest_main)
    add_test(NAME xhttp_test COMMAND xhttp_test)
  endif()
endif()
