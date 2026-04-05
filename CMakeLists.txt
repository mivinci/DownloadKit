cmake_minimum_required(VERSION 3.14)
project(xkit)

include("cmake/Functions.cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

xk_version("${CMAKE_SOURCE_DIR}/version.txt")
message(STATUS "xKit: Building ${XK_VERSION}")

option(XK_BUILD_XKIT "Build xKit" ON)
option(XK_BUILD_TESTS "Build xKit tests" ON)

add_compile_options(-Wall -Wextra)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror>)

if(XK_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(xbase)
add_subdirectory(xlog)
add_subdirectory(xbuf)

find_package(Libcurl QUIET)
if(Libcurl_FOUND)
  message(STATUS "xKit: Found libcurl, building xhttp module")
  add_subdirectory(xhttp)
  add_subdirectory(examples)
else()
  message(STATUS "xKit: libcurl not found, skipping xhttp module")
endif()
