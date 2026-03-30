file(GLOB_RECURSE XBASE_HEADERS "*.h")
file(GLOB_RECURSE XBASE_SOURCES "*.c")
add_library(xbase SHARED ${XBASE_SOURCES} ${XBASE_HEADERS})

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
  add_executable(xbase_test ${XBASE_TEST_SOURCES})
  target_link_libraries(xbase_test PRIVATE xbase GTest::gtest GTest::gtest_main)
  add_test(NAME xbase_test COMMAND xbase_test)
endif()
