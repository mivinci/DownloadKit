# FindGTest.cmake - Find and configure Google Test
#
# Result variables:
#   GTest_FOUND          - Whether GTest was found
#   GTest_INCLUDE_DIRS   - GTest include directories
#   GTest_LIBRARIES      - GTest library
#   GTest_MAIN_LIBRARIES - GTest main library
#
# Imported targets:
#   GTest::gtest         - GTest library target
#   GTest::gtest_main    - GTest main library target
#
# If GTest is not found on the system, it will be fetched and built
# from source via FetchContent.

# Prevent duplicate find
if(GTest_FOUND)
  return()
endif()

# ---- Method 1: Try CMake Config mode ----
find_package(GTest CONFIG QUIET)
if(GTest_FOUND)
  message(STATUS "FindGTest: Found GTest via CMake Config mode")
  return()
endif()

# ---- Method 2: Try pkg-config ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(_GTEST QUIET gtest)
  if(_GTEST_FOUND)
    set(GTest_INCLUDE_DIRS ${_GTEST_INCLUDE_DIRS})
    set(GTest_LIBRARIES    ${_GTEST_LIBRARIES})

    pkg_check_modules(_GTEST_MAIN QUIET gtest_main)
    if(_GTEST_MAIN_FOUND)
      set(GTest_MAIN_LIBRARIES ${_GTEST_MAIN_LIBRARIES})
    endif()
  endif()
endif()

# ---- Method 3: Manual search in common paths ----
if(NOT GTest_INCLUDE_DIRS)
  find_path(GTest_INCLUDE_DIRS
    NAMES gtest/gtest.h
    PATHS
      /usr/local/include
      /usr/include
      /opt/homebrew/include
      $ENV{GTEST_ROOT}/include
    NO_DEFAULT_PATH
  )
  if(NOT GTest_INCLUDE_DIRS)
    find_path(GTest_INCLUDE_DIRS NAMES gtest/gtest.h)
  endif()
endif()

if(NOT GTest_LIBRARIES)
  find_library(GTest_LIBRARIES
    NAMES gtest
    PATHS
      /usr/local/lib
      /usr/lib
      /opt/homebrew/lib
      $ENV{GTEST_ROOT}/lib
    NO_DEFAULT_PATH
  )
  if(NOT GTest_LIBRARIES)
    find_library(GTest_LIBRARIES NAMES gtest)
  endif()
endif()

if(NOT GTest_MAIN_LIBRARIES)
  find_library(GTest_MAIN_LIBRARIES
    NAMES gtest_main
    PATHS
      /usr/local/lib
      /usr/lib
      /opt/homebrew/lib
      $ENV{GTEST_ROOT}/lib
    NO_DEFAULT_PATH
  )
  if(NOT GTest_MAIN_LIBRARIES)
    find_library(GTest_MAIN_LIBRARIES NAMES gtest_main)
  endif()
endif()

# ---- Validate results ----
include(FindPackageHandleStandardArgs)

if(GTest_INCLUDE_DIRS AND GTest_LIBRARIES)
  find_package_handle_standard_args(GTest
    REQUIRED_VARS GTest_INCLUDE_DIRS GTest_LIBRARIES
  )
endif()

# ---- Create IMPORTED targets if found on system ----
if(GTest_FOUND)
  find_package(Threads QUIET)

  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES
      IMPORTED_LOCATION             "${GTest_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${GTest_INCLUDE_DIRS}"
    )
    if(Threads_FOUND)
      set_target_properties(GTest::gtest PROPERTIES
        INTERFACE_LINK_LIBRARIES Threads::Threads
      )
    endif()
  endif()

  if(GTest_MAIN_LIBRARIES AND NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest_main PROPERTIES
      IMPORTED_LOCATION             "${GTest_MAIN_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${GTest_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES      GTest::gtest
    )
  endif()

  mark_as_advanced(GTest_INCLUDE_DIRS GTest_LIBRARIES GTest_MAIN_LIBRARIES)
  message(STATUS "FindGTest: Found system GTest")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindGTest: System GTest not found, fetching from source")

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
  )
  set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # Create an imported interface target wrapping the static lib
  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest INTERFACE IMPORTED GLOBAL)
    set_target_properties(GTest::gtest PROPERTIES
      INTERFACE_LINK_LIBRARIES gtest
    )
  endif()
  if(NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main INTERFACE IMPORTED GLOBAL)
    set_target_properties(GTest::gtest_main PROPERTIES
      INTERFACE_LINK_LIBRARIES gtest_main
    )
  endif()

  set(GTest_FOUND TRUE)
  message(STATUS "FindGTest: Built GTest from source")
endif()
