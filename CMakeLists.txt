cmake_minimum_required(VERSION 3.14)
project(xKit VERSION 0.0.1 LANGUAGES C CXX)

include("cmake/Functions.cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Pre-release channel (alpha / beta / rc / "")
set(XK_VERSION_CHANNEL "alpha")
set(XK_VERSION_BUILDNO 1)

if(XK_VERSION_CHANNEL)
  set(XK_VERSION "${PROJECT_VERSION}-${XK_VERSION_CHANNEL}.${XK_VERSION_BUILDNO}")
else()
  set(XK_VERSION "${PROJECT_VERSION}")
endif()

message(STATUS "xKit: Building ${XK_VERSION}")

option(XK_BUILD_STATIC "Build xKit as static libraries" OFF)
option(XK_BUILD_TESTS "Build xKit tests" ON)
option(XK_BUILD_BENCHMARKS "Build xKit benchmarks" ON)
option(XK_BUILD_EXAMPLES "Build xKit example programs" OFF)
option(XK_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(XK_DEBUG_LEVEL "Debug logging level (0-3, higher means more verbose)" 0)

if(XK_BUILD_STATIC)
  set(XK_LIB_TYPE STATIC)
else()
  set(XK_LIB_TYPE SHARED)
endif()

if(MSVC)
  add_compile_options(/W3)
else()
  add_compile_options(-Wall -Wextra)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror>)
endif()

if(XK_ENABLE_ASAN AND NOT MSVC)
  message(STATUS "xKit: AddressSanitizer enabled")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# Pass debug level to source code
add_compile_definitions(XK_DEBUG_LEVEL=${XK_DEBUG_LEVEL})

if(XK_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

if(XK_BUILD_BENCHMARKS)
  find_package(GBenchmark REQUIRED)
endif()

find_package(Libcurl REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/modules)

add_subdirectory(modules/xbase)
add_subdirectory(modules/xlog)
add_subdirectory(modules/xbuf)
add_subdirectory(modules/xnet)
add_subdirectory(modules/xcrypto)
add_subdirectory(modules/xhttp)
add_subdirectory(modules/xp2p)
add_subdirectory(modules/xfer)
add_subdirectory(modules/xagent)
add_subdirectory(modules/xjs)
add_subdirectory(modules/xline)

if(XK_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(XK_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
