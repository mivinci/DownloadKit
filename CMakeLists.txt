cmake_minimum_required(VERSION 3.14)
project(xkit)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include("cmake/Functions.cmake")

xk_version("${CMAKE_SOURCE_DIR}/version.txt")
xk_message(STATUS "Building ${XK_VERSION}")

xk_option(XK_BUILD_XKIT "Build xKit" ON)
xk_option(XK_BUILD_TESTS "Build xKit tests" ON)

if(XK_BUILD_TESTS)
  enable_testing()
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
  find_package(GTest REQUIRED)
endif()

include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(xbase)
