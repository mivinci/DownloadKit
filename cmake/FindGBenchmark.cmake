# FindGBenchmark.cmake - Find and configure local Google Benchmark
#
# Result variables:
#   GBenchmark_FOUND            - Whether Google Benchmark was found
#   GBenchmark_INCLUDE_DIRS     - Google Benchmark include directories
#   GBenchmark_LIBRARIES        - Google Benchmark library
#   GBenchmark_MAIN_LIBRARIES   - Google Benchmark main library
#
# Imported targets:
#   GBenchmark::benchmark       - Google Benchmark library target
#   GBenchmark::benchmark_main  - Google Benchmark main library target

# Prevent redundant searches
if(GBenchmark_FOUND)
  return()
endif()

# ---- Method 1: Try CMake Config mode ----
find_package(benchmark CONFIG QUIET)
if(benchmark_FOUND)
  message(STATUS "FindGBenchmark: Found Google Benchmark via CMake Config mode")
  # Map config-mode targets to our namespaced targets
  if(TARGET benchmark::benchmark AND NOT TARGET GBenchmark::benchmark)
    add_library(GBenchmark::benchmark ALIAS benchmark::benchmark)
  endif()
  if(TARGET benchmark::benchmark_main AND NOT TARGET GBenchmark::benchmark_main)
    add_library(GBenchmark::benchmark_main ALIAS benchmark::benchmark_main)
  endif()
  set(GBenchmark_FOUND TRUE)
  return()
endif()

# ---- Method 2: Try pkg-config ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(_GBENCHMARK QUIET benchmark)
  if(_GBENCHMARK_FOUND)
    set(GBenchmark_INCLUDE_DIRS ${_GBENCHMARK_INCLUDE_DIRS})
    set(GBenchmark_LIBRARIES    ${_GBENCHMARK_LIBRARIES})

    pkg_check_modules(_GBENCHMARK_MAIN QUIET benchmark_main)
    if(_GBENCHMARK_MAIN_FOUND)
      set(GBenchmark_MAIN_LIBRARIES ${_GBENCHMARK_MAIN_LIBRARIES})
    endif()
  endif()
endif()

# ---- Method 3: Manual search in common paths ----
if(NOT GBenchmark_INCLUDE_DIRS)
  find_path(GBenchmark_INCLUDE_DIRS
    NAMES benchmark/benchmark.h
    PATHS
      /usr/local/include
      /usr/include
      /opt/homebrew/include
      $ENV{GBENCHMARK_ROOT}/include
    NO_DEFAULT_PATH
  )
  # Fallback to default search paths
  if(NOT GBenchmark_INCLUDE_DIRS)
    find_path(GBenchmark_INCLUDE_DIRS NAMES benchmark/benchmark.h)
  endif()
endif()

if(NOT GBenchmark_LIBRARIES)
  find_library(GBenchmark_LIBRARIES
    NAMES benchmark
    PATHS
      /usr/local/lib
      /usr/lib
      /opt/homebrew/lib
      $ENV{GBENCHMARK_ROOT}/lib
    NO_DEFAULT_PATH
  )
  if(NOT GBenchmark_LIBRARIES)
    find_library(GBenchmark_LIBRARIES NAMES benchmark)
  endif()
endif()

if(NOT GBenchmark_MAIN_LIBRARIES)
  find_library(GBenchmark_MAIN_LIBRARIES
    NAMES benchmark_main
    PATHS
      /usr/local/lib
      /usr/lib
      /opt/homebrew/lib
      $ENV{GBENCHMARK_ROOT}/lib
    NO_DEFAULT_PATH
  )
  if(NOT GBenchmark_MAIN_LIBRARIES)
    find_library(GBenchmark_MAIN_LIBRARIES NAMES benchmark_main)
  endif()
endif()

# ---- Validate results ----
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GBenchmark
  REQUIRED_VARS GBenchmark_INCLUDE_DIRS GBenchmark_LIBRARIES
)

# ---- Create IMPORTED targets ----
if(GBenchmark_FOUND)
  # Google Benchmark depends on threads
  find_package(Threads QUIET)

  if(NOT TARGET GBenchmark::benchmark)
    add_library(GBenchmark::benchmark UNKNOWN IMPORTED)
    set_target_properties(GBenchmark::benchmark PROPERTIES
      IMPORTED_LOCATION             "${GBenchmark_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${GBenchmark_INCLUDE_DIRS}"
    )
    if(Threads_FOUND)
      set_target_properties(GBenchmark::benchmark PROPERTIES
        INTERFACE_LINK_LIBRARIES Threads::Threads
      )
    endif()
  endif()

  if(GBenchmark_MAIN_LIBRARIES AND NOT TARGET GBenchmark::benchmark_main)
    add_library(GBenchmark::benchmark_main UNKNOWN IMPORTED)
    set_target_properties(GBenchmark::benchmark_main PROPERTIES
      IMPORTED_LOCATION             "${GBenchmark_MAIN_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${GBenchmark_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES      GBenchmark::benchmark
    )
  endif()

  mark_as_advanced(GBenchmark_INCLUDE_DIRS GBenchmark_LIBRARIES GBenchmark_MAIN_LIBRARIES)
endif()
