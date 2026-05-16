cmake_minimum_required(VERSION 3.14)
project(moo VERSION 0.0.1 LANGUAGES C CXX)

include("cmake/Functions.cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Pre-release channel (alpha / beta / rc / "")
set(MOO_VERSION_CHANNEL "alpha")
set(MOO_VERSION_BUILDNO 1)

if(MOO_VERSION_CHANNEL)
  set(MOO_VERSION "${PROJECT_VERSION}-${MOO_VERSION_CHANNEL}.${MOO_VERSION_BUILDNO}")
else()
  set(MOO_VERSION "${PROJECT_VERSION}")
endif()

message(STATUS "moo: Building ${MOO_VERSION}")

option(MOO_BUILD_STATIC "Build moo as static libraries" OFF)
option(MOO_BUILD_TESTS "Build moo tests" ON)
option(MOO_BUILD_BENCHMARKS "Build moo benchmarks" ON)
option(MOO_BUILD_APPS "Build moo applications (apps/)" OFF)
option(MOO_BUILD_EXAMPLES "Build moo example programs" OFF)
option(MOO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MOO_DEBUG_LEVEL "Debug logging level (0-3, higher means more verbose)" 0)

if(MOO_BUILD_STATIC)
  set(MOO_LIB_TYPE STATIC)
else()
  set(MOO_LIB_TYPE SHARED)
endif()

if(MSVC)
  add_compile_options(/W3)
else()
  add_compile_options(-Wall -Wextra)
  add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror>)
endif()

if(MOO_ENABLE_ASAN AND NOT MSVC)
  message(STATUS "moo: AddressSanitizer enabled")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()

# Pass debug level to source code
add_compile_definitions(MOO_DEBUG_LEVEL=${MOO_DEBUG_LEVEL})

if(MOO_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

if(MOO_BUILD_BENCHMARKS)
  find_package(GBenchmark REQUIRED)
endif()

find_package(Libcurl REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/libx)

add_subdirectory(libx/xbase)
add_subdirectory(libx/xlog)
add_subdirectory(libx/xbuf)
add_subdirectory(libx/xnet)
add_subdirectory(libx/xcrypto)
add_subdirectory(libx/xhttp)
add_subdirectory(libx/xp2p)
add_subdirectory(libx/xfer)
add_subdirectory(libx/xagent)
add_subdirectory(libx/xjs)
add_subdirectory(libx/xline)
add_subdirectory(libx/xtui)
add_subdirectory(libxpp)

if(MOO_BUILD_APPS)
  add_subdirectory(cli)
endif()

if(MOO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MOO_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
