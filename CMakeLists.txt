cmake_minimum_required(VERSION 3.14)
project(xkit)

include("cmake/Functions.cmake")

xk_version("${CMAKE_SOURCE_DIR}/version")
xk_message(STATUS "Building ${XK_VERSION}")

xk_option(XK_BUILD_XKIT "Build xkit" ON)
xk_option(XK_BUILD_EXAMPLES "Build xkit examples" OFF)

if(UNIX)
  add_compile_options(-fvisibility=hidden)
endif()

include_directories(${CMAKE_SOURCE_DIR})
add_subdirectory(xbase)

if(XK_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
