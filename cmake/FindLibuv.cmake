# FindLibuv.cmake - Find libuv library
#
# This module defines:
#   Libuv_FOUND        - True if libuv was found
#   Libuv_INCLUDE_DIRS - Include directories for libuv
#   Libuv_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Libuv::Libuv       - The libuv library

include(FindPackageHandleStandardArgs)

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

find_package_handle_standard_args(Libuv
  REQUIRED_VARS Libuv_LIBRARY Libuv_INCLUDE_DIR
)

if(Libuv_FOUND)
  set(Libuv_INCLUDE_DIRS ${Libuv_INCLUDE_DIR})
  set(Libuv_LIBRARIES    ${Libuv_LIBRARY})

  if(NOT TARGET Libuv::Libuv)
    add_library(Libuv::Libuv UNKNOWN IMPORTED)
    set_target_properties(Libuv::Libuv PROPERTIES
      IMPORTED_LOCATION             "${Libuv_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Libuv_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(Libuv_INCLUDE_DIR Libuv_LIBRARY)
