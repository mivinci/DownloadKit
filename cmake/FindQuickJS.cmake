# FindQuickJS.cmake - Find QuickJS library
#
# This module defines:
#   QuickJS_FOUND        - True if QuickJS was found
#   QuickJS_INCLUDE_DIRS - Include directories for QuickJS
#   QuickJS_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   QuickJS::QuickJS     - The QuickJS library
#
# If QuickJS is not found on the system, it will be fetched and built
# from source via FetchContent.
#
# We track the quickjs-ng fork (https://github.com/quickjs-ng/quickjs)
# rather than bellard's upstream: it ships a proper CMake build system,
# has an active release cadence, exposes more public API (Symbol / Date
# helpers we need in xjs), and carries accumulated bug fixes.

# Prevent duplicate find
if(QuickJS_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed quickjs-ng first ----
find_path(QuickJS_INCLUDE_DIR
  NAMES quickjs.h
  PATH_SUFFIXES quickjs
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(QuickJS_LIBRARY
  NAMES qjs quickjs
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(QuickJS_INCLUDE_DIR AND QuickJS_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(QuickJS
    REQUIRED_VARS QuickJS_LIBRARY QuickJS_INCLUDE_DIR
  )

  set(QuickJS_INCLUDE_DIRS ${QuickJS_INCLUDE_DIR})
  set(QuickJS_LIBRARIES    ${QuickJS_LIBRARY})

  if(NOT TARGET QuickJS::QuickJS)
    add_library(QuickJS::QuickJS UNKNOWN IMPORTED)
    set_target_properties(QuickJS::QuickJS PROPERTIES
      IMPORTED_LOCATION             "${QuickJS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${QuickJS_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(QuickJS_INCLUDE_DIR QuickJS_LIBRARY)
  message(STATUS "FindQuickJS: Found system quickjs: ${QuickJS_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindQuickJS: System quickjs not found, fetching from source")

  include(FetchContent)
  xk_github_url(quickjs-ng/quickjs v0.14.0 _quickjs_url)
  FetchContent_Declare(
    quickjs
    URL ${_quickjs_url}
  )

  # Turn off everything we don't need from quickjs-ng:
  #   - the qjs / qjsc CLI executables (not option-guarded upstream;
  #     we rely on EXCLUDE_FROM_ALL below to keep them out of `all`)
  #   - libc host bindings (xjs provides its own)
  #   - examples, installation rules, -Werror
  set(BUILD_SHARED_LIBS OFF    CACHE BOOL "" FORCE)
  set(QJS_BUILD_LIBC    OFF    CACHE BOOL "" FORCE)
  set(QJS_BUILD_EXAMPLES OFF   CACHE BOOL "" FORCE)
  set(QJS_BUILD_WERROR  OFF    CACHE BOOL "" FORCE)
  set(QJS_ENABLE_INSTALL OFF   CACHE BOOL "" FORCE)

  # quickjs-ng unconditionally declares qjs_exe / qjsc / api-test /
  # lre-test targets.  Using EXCLUDE_FROM_ALL on the subdirectory means
  # those executables are only built if something explicitly depends on
  # them — which nothing in xkit does, so they stay out of our build.
  FetchContent_GetProperties(quickjs)
  if(NOT quickjs_POPULATED)
    FetchContent_Populate(quickjs)
    add_subdirectory(${quickjs_SOURCE_DIR} ${quickjs_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()

  # Suppress warnings-as-errors inherited from the parent project and
  # enable PIC so the static lib can be linked into shared libraries
  # (e.g. a future libxjs.so).  quickjs-ng already marks qjs PUBLIC
  # include dirs, so we don't need to re-declare them here.
  if(TARGET qjs)
    target_compile_options(qjs PRIVATE -w -Wno-error)
    set_target_properties(qjs PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create an imported interface target wrapping the static lib
  if(NOT TARGET QuickJS::QuickJS)
    add_library(QuickJS::QuickJS INTERFACE IMPORTED GLOBAL)
    set_target_properties(QuickJS::QuickJS PROPERTIES
      INTERFACE_LINK_LIBRARIES qjs
    )
  endif()

  set(QuickJS_INCLUDE_DIRS ${quickjs_SOURCE_DIR})
  set(QuickJS_LIBRARIES    qjs)
  set(QuickJS_FOUND TRUE)

  message(STATUS "FindQuickJS: Built quickjs-ng from source (v0.14.0)")
endif()

mark_as_advanced(QuickJS_INCLUDE_DIR QuickJS_LIBRARY)
