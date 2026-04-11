cmake_minimum_required(VERSION 3.14)
project(xKit)

include("cmake/Functions.cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

xk_version("${CMAKE_SOURCE_DIR}/version.txt")
message(STATUS "xKit: Building ${XK_VERSION}")

option(XK_BUILD_XKIT "Build xKit" ON)
option(XK_BUILD_TESTS "Build xKit tests" ON)
option(XK_BUILD_BENCHMARKS "Build xKit benchmarks" OFF)
option(XK_BUILD_EXAMPLES "Build xKit example programs" OFF)
option(XK_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(XK_ENABLE_DEBUG "Enable verbose debug logging in xKit modules" OFF)

add_compile_options(-Wall -Wextra)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror>)

if(XK_ENABLE_ASAN)
  message(STATUS "xKit: AddressSanitizer enabled")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

if(XK_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

if(XK_BUILD_BENCHMARKS)
  find_package(GBenchmark REQUIRED)
endif()

find_package(Libcurl REQUIRED)

include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(xbase)
add_subdirectory(xlog)
add_subdirectory(xbuf)
add_subdirectory(xnet)
add_subdirectory(xhttp)
add_subdirectory(xp2p)

if(XK_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(XK_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
