# FindNghttp3.cmake - Find nghttp3 (HTTP/3)
#
# This module defines:
#   Nghttp3_FOUND        - True if nghttp3 was found
#   Nghttp3_INCLUDE_DIRS - Include directories
#   Nghttp3_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Nghttp3::Nghttp3     - The nghttp3 library

if(Nghttp3_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

find_path(Nghttp3_INCLUDE_DIR
  NAMES nghttp3/nghttp3.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
    /opt/homebrew/opt/nghttp3/include
)

find_library(Nghttp3_LIBRARY
  NAMES nghttp3
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
    /opt/homebrew/opt/nghttp3/lib
)

find_package_handle_standard_args(Nghttp3
  REQUIRED_VARS Nghttp3_LIBRARY Nghttp3_INCLUDE_DIR
)

if(Nghttp3_FOUND)
  set(Nghttp3_INCLUDE_DIRS ${Nghttp3_INCLUDE_DIR})
  set(Nghttp3_LIBRARIES    ${Nghttp3_LIBRARY})

  if(NOT TARGET Nghttp3::Nghttp3)
    add_library(Nghttp3::Nghttp3 UNKNOWN IMPORTED)
    set_target_properties(Nghttp3::Nghttp3 PROPERTIES
      IMPORTED_LOCATION             "${Nghttp3_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Nghttp3_INCLUDE_DIR}"
    )
  endif()

  message(STATUS "FindNghttp3: Found nghttp3 ${Nghttp3_LIBRARY}")
else()
  message(STATUS "FindNghttp3: nghttp3 not found (HTTP/3 client disabled)")
endif()

mark_as_advanced(Nghttp3_INCLUDE_DIR Nghttp3_LIBRARY)
