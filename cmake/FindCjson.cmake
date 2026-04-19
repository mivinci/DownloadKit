# FindCjson.cmake - Find cJSON library
#
# This module defines:
#   Cjson_FOUND        - True if cJSON was found
#   Cjson_INCLUDE_DIRS - Include directories for cJSON
#   Cjson_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Cjson::Cjson       - The cJSON library
#
# If cJSON is not found on the system, it will be fetched and built
# from source via FetchContent.

# Prevent duplicate find
if(Cjson_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed cJSON first ----
find_path(Cjson_INCLUDE_DIR
  NAMES cJSON.h
  PATH_SUFFIXES cjson
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Cjson_LIBRARY
  NAMES cjson
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Cjson_INCLUDE_DIR AND Cjson_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Cjson
    REQUIRED_VARS Cjson_LIBRARY Cjson_INCLUDE_DIR
  )

  set(Cjson_INCLUDE_DIRS ${Cjson_INCLUDE_DIR})
  set(Cjson_LIBRARIES    ${Cjson_LIBRARY})

  if(NOT TARGET Cjson::Cjson)
    add_library(Cjson::Cjson UNKNOWN IMPORTED)
    set_target_properties(Cjson::Cjson PROPERTIES
      IMPORTED_LOCATION             "${Cjson_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Cjson_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Cjson_INCLUDE_DIR Cjson_LIBRARY)
  message(STATUS "FindCjson: Found system cJSON: ${Cjson_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindCjson: System cJSON not found, fetching from source")

  include(FetchContent)
  FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
  )
  set(ENABLE_CJSON_TEST    OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  # cJSON v1.7.18 uses cmake_minimum_required(VERSION 3.0) which is rejected
  # by CMake ≥ 3.30. Allow the old policy version so FetchContent can proceed.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_MakeAvailable(cjson)
  unset(CMAKE_POLICY_VERSION_MINIMUM)

  # Suppress warnings-as-errors inherited from parent project
  # and enable PIC for linking into shared libraries
  if(TARGET cjson)
    target_compile_options(cjson PRIVATE -Wno-error)
    set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create an imported interface target wrapping the static lib
  if(NOT TARGET Cjson::Cjson)
    add_library(Cjson::Cjson INTERFACE IMPORTED GLOBAL)
    set_target_properties(Cjson::Cjson PROPERTIES
      INTERFACE_LINK_LIBRARIES cjson
      INTERFACE_INCLUDE_DIRECTORIES "${cjson_SOURCE_DIR}"
    )
  endif()

  set(Cjson_INCLUDE_DIRS ${cjson_SOURCE_DIR})
  set(Cjson_LIBRARIES    cjson)
  set(Cjson_FOUND TRUE)

  message(STATUS "FindCjson: Built cJSON from source")
endif()
