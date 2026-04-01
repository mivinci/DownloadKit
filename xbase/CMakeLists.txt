file(GLOB_RECURSE XBASE_HEADERS "*.h")
file(GLOB_RECURSE XBASE_SOURCES "*.c")
# Exclude curl.c from the default glob; it is added only when libcurl is available.
list(FILTER XBASE_SOURCES EXCLUDE REGEX ".*/curl\\.c$")
add_library(xbase SHARED ${XBASE_SOURCES} ${XBASE_HEADERS})

if(Libcurl_FOUND)
  target_sources(xbase PRIVATE ${CMAKE_SOURCE_DIR}/xbase/curl.c)
  target_compile_definitions(xbase PRIVATE XK_HAS_CURL)
  target_link_libraries(xbase PRIVATE Libcurl::Libcurl)
endif()

# ── Event backend detection (kqueue → epoll → poll fallback) ──
include(CheckIncludeFile)
check_include_file(sys/event.h HAS_SYS_EVENT_H)
check_include_file(sys/epoll.h HAS_SYS_EPOLL_H)

if(HAS_SYS_EVENT_H)
  target_compile_definitions(xbase PRIVATE XK_HAS_KQUEUE)
  message(STATUS "xbase: event backend = kqueue")
elseif(HAS_SYS_EPOLL_H)
  target_compile_definitions(xbase PRIVATE XK_HAS_EPOLL)
  message(STATUS "xbase: event backend = epoll")
else()
  message(STATUS "xbase: event backend = poll (POSIX fallback)")
endif()

# ── Backtrace backend detection (libunwind → execinfo → stub) ──
find_package(Libunwind QUIET)

if(Libunwind_FOUND)
  target_compile_definitions(xbase PRIVATE XK_HAS_LIBUNWIND)
  target_link_libraries(xbase PRIVATE Libunwind::Libunwind)
  message(STATUS "xbase: backtrace backend = libunwind")
else()
  include(CheckIncludeFile)
  check_include_file(execinfo.h HAS_EXECINFO_H)

  if(HAS_EXECINFO_H)
    target_compile_definitions(xbase PRIVATE XK_HAS_EXECINFO)
    message(STATUS "xbase: backtrace backend = execinfo")

    # On Linux, -rdynamic is needed for backtrace_symbols() to resolve names
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(xbase PUBLIC -rdynamic)
    endif()
  else()
    message(STATUS "xbase: backtrace backend = stub (no libunwind or execinfo)")
  endif()
endif()

if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XBASE_TEST_SOURCES "*_test.cpp")
  # curl_test.cpp has its own main() and requires libcurl — handle separately.
  list(FILTER XBASE_TEST_SOURCES EXCLUDE REGEX ".*/curl_test\\.cpp$")

  add_executable(xbase_test ${XBASE_TEST_SOURCES})
  target_link_libraries(xbase_test PRIVATE xbase GTest::gtest GTest::gtest_main)
  add_test(NAME xbase_test COMMAND xbase_test)

  if(Libcurl_FOUND)
    add_executable(xbase_curl_test ${CMAKE_SOURCE_DIR}/xbase/curl_test.cpp)
    target_link_libraries(xbase_curl_test PRIVATE xbase GTest::gtest GTest::gtest_main Libcurl::Libcurl)
    add_test(NAME xbase_curl_test COMMAND xbase_curl_test)
  endif()
endif()
