file(GLOB_RECURSE XBASE_HEADERS "*.h")
file(GLOB_RECURSE XBASE_SOURCES "*.c")

# ── Exclude POSIX-only sources on Windows ──
if(WIN32)
  list(FILTER XBASE_SOURCES EXCLUDE REGEX
    "event_epoll\\.c$|event_kqueue\\.c$|event_poll\\.c$|socket\\.c$")
endif()

# On Windows, always build as static library to simplify linking with tests
if(WIN32)
  add_library(xbase STATIC ${XBASE_SOURCES} ${XBASE_HEADERS})
else()
  add_library(xbase ${XK_LIB_TYPE} ${XBASE_SOURCES} ${XBASE_HEADERS})
endif()

if(WIN32)
  target_compile_definitions(xbase PRIVATE _WIN32_WINNT=0x0601 _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(xbase PRIVATE kernel32 ws2_32)
endif()

# ── Event backend detection (kqueue → epoll → poll fallback) ──
if(NOT WIN32)
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
else()
  message(STATUS "xbase: event backend = WSAPoll (Windows)")
endif()

# ── PTY support (forkpty) ──
# macOS: <util.h>, Linux: <pty.h> + -lutil
if(NOT WIN32)
  include(CheckIncludeFile)
  check_include_file(util.h HAS_UTIL_H)
  check_include_file(pty.h HAS_PTY_H)
  if(HAS_UTIL_H OR HAS_PTY_H)
    target_compile_definitions(xbase PRIVATE XK_HAS_PTY)
    if(HAS_UTIL_H)
      target_compile_definitions(xbase PRIVATE XK_HAS_UTIL_H)
    endif()
    if(HAS_PTY_H)
      target_compile_definitions(xbase PRIVATE XK_HAS_PTY_H)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_libraries(xbase PRIVATE util)
    endif()
    message(STATUS "xbase: PTY support = yes")
  else()
    message(STATUS "xbase: PTY support = no")
  endif()
else()
  message(STATUS "xbase: PTY support = no (Windows)")
endif()

# ── Backtrace backend detection (libunwind → execinfo → stub) ──
if(NOT WIN32)
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
else()
  message(STATUS "xbase: backtrace backend = stub (Windows)")
endif()

if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XBASE_TEST_SOURCES "*_test.cpp")
  add_executable(xbase_test ${XBASE_TEST_SOURCES})
  target_link_libraries(xbase_test PRIVATE xbase GTest::gtest_main)
  if(WIN32)
    target_link_libraries(xbase_test PRIVATE ws2_32 kernel32)
    target_compile_options(xbase_test PRIVATE /std:c++20)
    target_compile_definitions(xbase_test PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME xbase_test COMMAND xbase_test)
endif()

if(XK_BUILD_BENCHMARKS)
  find_package(Libuv QUIET)

  file(GLOB XBASE_BENCH_SOURCES "*_bench.cpp")
  foreach(src ${XBASE_BENCH_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    xk_add_benchmark(${name}
      SOURCES ${src}
      LIBS    xbase
    )

    # Link libuv for baseline comparison
    if((name STREQUAL "task_bench" OR name STREQUAL "event_bench") AND Libuv_FOUND)
      target_link_libraries(${name} PRIVATE Libuv::Libuv)
      target_compile_definitions(${name} PRIVATE XK_HAS_LIBUV)
      message(STATUS "xbase: ${name} will include libuv baseline")
    endif()
  endforeach()
endif()
