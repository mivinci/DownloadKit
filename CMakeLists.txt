cmake_minimum_required(VERSION 3.14)
project(xKit)

include("cmake/Functions.cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

xk_version("${CMAKE_SOURCE_DIR}/version.txt")
message(STATUS "xKit: Building ${XK_VERSION}")

option(XK_BUILD_XKIT "Build xKit" ON)
option(XK_BUILD_TESTS "Build xKit tests" ON)
option(XK_BUILD_BENCHMARKS "Build xKit benchmarks" ON)
option(XK_BUILD_EXAMPLES "Build xKit example programs" OFF)
option(XK_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(XK_DEBUG_LEVEL "Debug logging level (0-3, higher means more verbose)" 0)

add_compile_options(-Wall -Wextra)
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror>)

if(XK_ENABLE_ASAN)
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

if(XK_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(XK_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
