# FindLibcurl.cmake - Find libcurl library
#
# This module defines:
#   Libcurl_FOUND        - True if libcurl was found
#   Libcurl_INCLUDE_DIRS - Include directories for libcurl
#   Libcurl_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Libcurl::Libcurl     - The libcurl library

include(FindPackageHandleStandardArgs)

find_path(Libcurl_INCLUDE_DIR
  NAMES curl/curl.h curl.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/homebrew/include
    /mingw64/include
)

find_library(Libcurl_LIBRARY
  NAMES curl
  PATHS
    /usr/lib
    /usr/lib64
    /usr/local/lib
    /usr/local/lib64
    /opt/homebrew/lib
    /mingw64/lib
)

find_package_handle_standard_args(Libcurl
  REQUIRED_VARS Libcurl_LIBRARY Libcurl_INCLUDE_DIR
)

if(Libcurl_FOUND)
  set(Libcurl_INCLUDE_DIRS ${Libcurl_INCLUDE_DIR})
  set(Libcurl_LIBRARIES    ${Libcurl_LIBRARY})

  if(NOT TARGET Libcurl::Libcurl)
    add_library(Libcurl::Libcurl UNKNOWN IMPORTED)
    set_target_properties(Libcurl::Libcurl PROPERTIES
      IMPORTED_LOCATION             "${Libcurl_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Libcurl_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(Libcurl_INCLUDE_DIR Libcurl_LIBRARY)
