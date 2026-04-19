# FindLibunwind.cmake - Find libunwind library
#
# This module defines:
#   Libunwind_FOUND        - True if libunwind was found
#   Libunwind_INCLUDE_DIRS - Include directories for libunwind
#   Libunwind_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Libunwind::Libunwind   - The libunwind library
#
# Note: libunwind uses autotools and contains platform-specific assembly,
# making it impractical to build from source via FetchContent.
# Install via your system package manager (e.g. apt install libunwind-dev).

# Prevent duplicate find
if(Libunwind_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

find_path(Libunwind_INCLUDE_DIR
  NAMES libunwind.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
)

find_library(Libunwind_LIBRARY
  NAMES unwind
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
)

find_package_handle_standard_args(Libunwind
  REQUIRED_VARS Libunwind_LIBRARY Libunwind_INCLUDE_DIR
)

if(Libunwind_FOUND)
  set(Libunwind_INCLUDE_DIRS ${Libunwind_INCLUDE_DIR})
  set(Libunwind_LIBRARIES    ${Libunwind_LIBRARY})

  if(NOT TARGET Libunwind::Libunwind)
    add_library(Libunwind::Libunwind UNKNOWN IMPORTED)
    set_target_properties(Libunwind::Libunwind PROPERTIES
      IMPORTED_LOCATION             "${Libunwind_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Libunwind_INCLUDE_DIR}"
    )
  endif()

  message(STATUS "FindLibunwind: Found system libunwind: ${Libunwind_LIBRARY}")
endif()

mark_as_advanced(Libunwind_INCLUDE_DIR Libunwind_LIBRARY)
