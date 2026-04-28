# FindLlhttp.cmake - Find llhttp library
#
# This module defines:
#   Llhttp_FOUND        - True if llhttp was found
#   Llhttp_INCLUDE_DIRS - Include directories for llhttp
#   Llhttp_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Llhttp::Llhttp      - The llhttp library
#
# If llhttp is not found on the system, it will be fetched and built
# from source via FetchContent.

# Prevent duplicate find
if(Llhttp_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed llhttp first ----
find_path(Llhttp_INCLUDE_DIR
  NAMES llhttp.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Llhttp_LIBRARY
  NAMES llhttp
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Llhttp_INCLUDE_DIR AND Llhttp_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Llhttp
    REQUIRED_VARS Llhttp_LIBRARY Llhttp_INCLUDE_DIR
  )

  set(Llhttp_INCLUDE_DIRS ${Llhttp_INCLUDE_DIR})
  set(Llhttp_LIBRARIES    ${Llhttp_LIBRARY})

  if(NOT TARGET Llhttp::Llhttp)
    add_library(Llhttp::Llhttp UNKNOWN IMPORTED)
    set_target_properties(Llhttp::Llhttp PROPERTIES
      IMPORTED_LOCATION             "${Llhttp_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Llhttp_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Llhttp_INCLUDE_DIR Llhttp_LIBRARY)
  message(STATUS "FindLlhttp: Found system llhttp: ${Llhttp_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindLlhttp: System llhttp not found, fetching from source")

  include(FetchContent)
  xk_github_url(nodejs/llhttp v9.2.1 _llhttp_url)
  FetchContent_Declare(
    llhttp
    URL ${_llhttp_url}
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(llhttp)

  # Suppress warnings-as-errors inherited from parent project
  # and enable PIC for linking into shared libraries
  if(TARGET llhttp_static)
    target_compile_options(llhttp_static PRIVATE -Wno-error -Wno-unused-parameter)
    set_target_properties(llhttp_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create an imported interface target wrapping the static lib
  if(NOT TARGET Llhttp::Llhttp)
    add_library(Llhttp::Llhttp INTERFACE IMPORTED GLOBAL)
    set_target_properties(Llhttp::Llhttp PROPERTIES
      INTERFACE_LINK_LIBRARIES llhttp_static
      INTERFACE_INCLUDE_DIRECTORIES "${llhttp_SOURCE_DIR}/include"
    )
  endif()

  set(Llhttp_INCLUDE_DIRS ${llhttp_SOURCE_DIR}/include)
  set(Llhttp_LIBRARIES    llhttp_static)
  set(Llhttp_FOUND TRUE)

  message(STATUS "FindLlhttp: Built llhttp from source")
endif()

mark_as_advanced(Llhttp_INCLUDE_DIR Llhttp_LIBRARY)
