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

# moo-app-level toggles. Library-build toggles (tests, benchmarks, TLS
# backend, debug level, static vs shared, ...) live in libx/CMakeLists.txt
# under the X_* namespace.
option(MOO_BUILD_APPS  "Build moo applications (apps/)" OFF)
option(MOO_ENABLE_ASAN "Enable AddressSanitizer"        OFF)

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

find_package(Libcurl REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/libx)

# enable_testing() must be called from the top-level CMakeLists.txt for
# ctest to discover add_test() calls in sub-projects (libx, libx++).
# CMake scopes enable_testing() to the current directory and below, so
# calling it inside libx/CMakeLists.txt only would silently drop libx++
# tests. Cheap to call unconditionally; the X_BUILD_TESTS / XPP_BUILD_TESTS
# switches still gate whether any add_test() calls happen at all.
enable_testing()

add_subdirectory(libx)
add_subdirectory(libx++)

if(MOO_BUILD_APPS)
  add_subdirectory(cli)
endif()
