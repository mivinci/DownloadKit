# FindNghttp2.cmake - Find nghttp2 library
#
# This module defines:
#   Nghttp2_FOUND        - True if nghttp2 was found
#   Nghttp2_INCLUDE_DIRS - Include directories for nghttp2
#   Nghttp2_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Nghttp2::Nghttp2     - The nghttp2 library
#
# If nghttp2 is not found on the system, it will be fetched and built
# from source via FetchContent.

# Prevent duplicate find
if(Nghttp2_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed nghttp2 first ----
find_path(Nghttp2_INCLUDE_DIR
  NAMES nghttp2/nghttp2.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Nghttp2_LIBRARY
  NAMES nghttp2
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Nghttp2_INCLUDE_DIR AND Nghttp2_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Nghttp2
    REQUIRED_VARS Nghttp2_LIBRARY Nghttp2_INCLUDE_DIR
  )

  set(Nghttp2_INCLUDE_DIRS ${Nghttp2_INCLUDE_DIR})
  set(Nghttp2_LIBRARIES    ${Nghttp2_LIBRARY})

  if(NOT TARGET Nghttp2::Nghttp2)
    add_library(Nghttp2::Nghttp2 UNKNOWN IMPORTED)
    set_target_properties(Nghttp2::Nghttp2 PROPERTIES
      IMPORTED_LOCATION             "${Nghttp2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Nghttp2_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Nghttp2_INCLUDE_DIR Nghttp2_LIBRARY)
  message(STATUS "FindNghttp2: Found system nghttp2: ${Nghttp2_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindNghttp2: System nghttp2 not found, fetching from source")

  include(FetchContent)
  FetchContent_Declare(
    nghttp2
    GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
    GIT_TAG        v1.64.0
    GIT_SHALLOW    TRUE
  )
  set(ENABLE_LIB_ONLY    ON  CACHE BOOL "" FORCE)
  set(ENABLE_STATIC_LIB  ON  CACHE BOOL "" FORCE)
  set(ENABLE_SHARED_LIB  OFF CACHE BOOL "" FORCE)
  set(ENABLE_DOC         OFF CACHE BOOL "" FORCE)
  set(ENABLE_APP         OFF CACHE BOOL "" FORCE)
  set(ENABLE_HPACK_TOOLS OFF CACHE BOOL "" FORCE)
  set(ENABLE_EXAMPLES    OFF CACHE BOOL "" FORCE)
  set(WITH_LIBXML2       OFF CACHE BOOL "" FORCE)
  set(WITH_JEMALLOC      OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(nghttp2)

  # Suppress warnings-as-errors inherited from parent project
  # and enable PIC for linking into shared libraries
  if(TARGET nghttp2_static)
    target_compile_options(nghttp2_static PRIVATE -Wno-error)
    set_target_properties(nghttp2_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create an imported interface target wrapping the static lib
  if(NOT TARGET Nghttp2::Nghttp2)
    add_library(Nghttp2::Nghttp2 INTERFACE IMPORTED GLOBAL)
    set_target_properties(Nghttp2::Nghttp2 PROPERTIES
      INTERFACE_LINK_LIBRARIES nghttp2_static
      INTERFACE_INCLUDE_DIRECTORIES "${nghttp2_SOURCE_DIR}/lib/includes;${nghttp2_BINARY_DIR}/lib/includes"
    )
  endif()

  set(Nghttp2_INCLUDE_DIRS "${nghttp2_SOURCE_DIR}/lib/includes" "${nghttp2_BINARY_DIR}/lib/includes")
  set(Nghttp2_LIBRARIES    nghttp2_static)
  set(Nghttp2_FOUND TRUE)

  message(STATUS "FindNghttp2: Built nghttp2 from source")
endif()

mark_as_advanced(Nghttp2_INCLUDE_DIR Nghttp2_LIBRARY)
