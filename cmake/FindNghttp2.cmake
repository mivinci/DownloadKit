# FindNghttp2.cmake - Find nghttp2 library
#
# This module defines:
#   Nghttp2_FOUND        - True if nghttp2 was found
#   Nghttp2_INCLUDE_DIRS - Include directories for nghttp2
#   Nghttp2_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Nghttp2::Nghttp2     - The nghttp2 library

include(FindPackageHandleStandardArgs)

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

find_package_handle_standard_args(Nghttp2
  REQUIRED_VARS Nghttp2_LIBRARY Nghttp2_INCLUDE_DIR
)

if(Nghttp2_FOUND)
  set(Nghttp2_INCLUDE_DIRS ${Nghttp2_INCLUDE_DIR})
  set(Nghttp2_LIBRARIES    ${Nghttp2_LIBRARY})

  if(NOT TARGET Nghttp2::Nghttp2)
    add_library(Nghttp2::Nghttp2 UNKNOWN IMPORTED)
    set_target_properties(Nghttp2::Nghttp2 PROPERTIES
      IMPORTED_LOCATION             "${Nghttp2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Nghttp2_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(Nghttp2_INCLUDE_DIR Nghttp2_LIBRARY)
