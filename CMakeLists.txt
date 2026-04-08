cmake_minimum_required(VERSION 3.14)
project(xkit)

include("cmake/Functions.cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

xk_version("${CMAKE_SOURCE_DIR}/version.txt")
message(STATUS "xKit: Building ${XK_VERSION}")

option(XK_BUILD_XKIT "Build xKit" ON)
option(XK_BUILD_TESTS "Build xKit tests" ON)
option(XK_BUILD_BENCHMARKS "Build xKit benchmarks" OFF)
option(XK_ENABLE_ASAN "Enable AddressSanitizer" OFF)

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

include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(xbase)
add_subdirectory(xlog)
add_subdirectory(xbuf)
add_subdirectory(xnet)

find_package(Libcurl QUIET)
if(Libcurl_FOUND)
  message(STATUS "xKit: Found libcurl, building xhttp module")
  add_subdirectory(xhttp)
  add_subdirectory(examples)
else()
  message(STATUS "xKit: libcurl not found, skipping xhttp module")
endif()

if(XK_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
