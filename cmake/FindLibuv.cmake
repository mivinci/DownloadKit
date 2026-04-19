# FindLibuv.cmake - Find libuv library
#
# This module defines:
#   Libuv_FOUND        - True if libuv was found
#   Libuv_INCLUDE_DIRS - Include directories for libuv
#   Libuv_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Libuv::Libuv       - The libuv library
#
# If libuv is not found on the system, it will be fetched and built
# from source via FetchContent.

# Prevent duplicate find
if(Libuv_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed libuv first ----
find_path(Libuv_INCLUDE_DIR
  NAMES uv.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Libuv_LIBRARY
  NAMES uv
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Libuv_INCLUDE_DIR AND Libuv_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Libuv
    REQUIRED_VARS Libuv_LIBRARY Libuv_INCLUDE_DIR
  )

  set(Libuv_INCLUDE_DIRS ${Libuv_INCLUDE_DIR})
  set(Libuv_LIBRARIES    ${Libuv_LIBRARY})

  if(NOT TARGET Libuv::Libuv)
    add_library(Libuv::Libuv UNKNOWN IMPORTED)
    set_target_properties(Libuv::Libuv PROPERTIES
      IMPORTED_LOCATION             "${Libuv_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Libuv_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Libuv_INCLUDE_DIR Libuv_LIBRARY)
  message(STATUS "FindLibuv: Found system libuv: ${Libuv_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindLibuv: System libuv not found, fetching from source")

  include(FetchContent)
  FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG        v1.50.0
    GIT_SHALLOW    TRUE
  )
  set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(libuv)

  # Suppress warnings-as-errors inherited from parent project
  # and enable PIC for linking into shared libraries
  if(TARGET uv_a)
    target_compile_options(uv_a PRIVATE -Wno-error)
    set_target_properties(uv_a PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()
  if(TARGET uv)
    target_compile_options(uv PRIVATE -Wno-error)
    set_target_properties(uv PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create an imported interface target wrapping the static lib
  if(NOT TARGET Libuv::Libuv)
    add_library(Libuv::Libuv INTERFACE IMPORTED GLOBAL)
    set_target_properties(Libuv::Libuv PROPERTIES
      INTERFACE_LINK_LIBRARIES uv_a
      INTERFACE_INCLUDE_DIRECTORIES "${libuv_SOURCE_DIR}/include"
    )
  endif()

  set(Libuv_INCLUDE_DIRS ${libuv_SOURCE_DIR}/include)
  set(Libuv_LIBRARIES    uv_a)
  set(Libuv_FOUND TRUE)

  message(STATUS "FindLibuv: Built libuv from source")
endif()

mark_as_advanced(Libuv_INCLUDE_DIR Libuv_LIBRARY)
