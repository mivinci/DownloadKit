# FindLlhttp.cmake - Find llhttp library
#
# This module defines:
#   Llhttp_FOUND        - True if llhttp was found
#   Llhttp_INCLUDE_DIRS - Include directories for llhttp
#   Llhttp_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Llhttp::Llhttp      - The llhttp library

include(FindPackageHandleStandardArgs)

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

find_package_handle_standard_args(Llhttp
  REQUIRED_VARS Llhttp_LIBRARY Llhttp_INCLUDE_DIR
)

if(Llhttp_FOUND)
  set(Llhttp_INCLUDE_DIRS ${Llhttp_INCLUDE_DIR})
  set(Llhttp_LIBRARIES    ${Llhttp_LIBRARY})

  if(NOT TARGET Llhttp::Llhttp)
    add_library(Llhttp::Llhttp UNKNOWN IMPORTED)
    set_target_properties(Llhttp::Llhttp PROPERTIES
      IMPORTED_LOCATION             "${Llhttp_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Llhttp_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(Llhttp_INCLUDE_DIR Llhttp_LIBRARY)
