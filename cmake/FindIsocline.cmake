# FindIsocline.cmake - Find the isocline line-editing library
#
# This module defines:
#   Isocline_FOUND        - True if isocline was found
#   Isocline_INCLUDE_DIRS - Include directories for isocline
#   Isocline_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Isocline::Isocline    - The isocline library
#
# If isocline is not found on the system, it will be fetched and built
# from source via FetchContent.
#
# Isocline is a tiny, portable (POSIX + native Win32 Console), pure-C
# readline replacement by Daan Leijen — used by the ai_session demo to
# provide CJK-aware line editing, history, and Ctrl-R search without
# pulling in ncurses / readline / GPL licensing.

# Prevent duplicate find
if(Isocline_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed isocline first ----
find_path(Isocline_INCLUDE_DIR
  NAMES isocline.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Isocline_LIBRARY
  NAMES isocline
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Isocline_INCLUDE_DIR AND Isocline_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Isocline
    REQUIRED_VARS Isocline_LIBRARY Isocline_INCLUDE_DIR
  )

  set(Isocline_INCLUDE_DIRS ${Isocline_INCLUDE_DIR})
  set(Isocline_LIBRARIES    ${Isocline_LIBRARY})

  if(NOT TARGET Isocline::Isocline)
    add_library(Isocline::Isocline UNKNOWN IMPORTED)
    set_target_properties(Isocline::Isocline PROPERTIES
      IMPORTED_LOCATION             "${Isocline_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Isocline_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Isocline_INCLUDE_DIR Isocline_LIBRARY)
  message(STATUS "FindIsocline: Found system isocline: ${Isocline_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindIsocline: System isocline not found, fetching from source")

  include(FetchContent)
  xk_github_url(daanx/isocline v1.1.0 _isocline_url)
  FetchContent_Declare(
    isocline
    URL ${_isocline_url}
    # Use extraction time for mtimes so a URL change forces a
    # rebuild (CMake >= 3.24 / policy CMP0135).
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )

  # Upstream options we need to pin:
  #   IC_BUILD_TESTS=OFF   -- don't build example/test_colors binaries
  #   IC_DEBUG_MSG=OFF     -- suppress the stderr debug hooks
  set(IC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(IC_DEBUG_MSG   OFF CACHE BOOL "" FORCE)

  # Isocline v1.1.0 uses cmake_minimum_required(VERSION 3.10) which is
  # fine for CMake ≥ 3.14; set the policy min just in case a newer
  # CMake raises the floor (mirrors FindCjson's guard).
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  FetchContent_MakeAvailable(isocline)
  unset(CMAKE_POLICY_VERSION_MINIMUM)

  # Upstream compiles with -Wall -Wextra -Wpedantic -Wsign-conversion
  # which is stricter than what our tree uses; and our tree adds
  # -Werror for C. Shield isocline from the parent's -Werror so a
  # benign upstream warning never breaks our build.
  #
  # Also suppress -Wvariadic-macro-arguments-omitted: with
  # IC_NO_DEBUG_MSG set, upstream's debug_msg() expands to (void)(0)
  # but the call sites pass a single format string — under C99 that
  # leaves the variadic parameter empty, which only C23 formally
  # allows. The warning is benign and there is no upstream fix in
  # v1.1.0.
  if(TARGET isocline)
    target_compile_options(isocline PRIVATE
      -Wno-error
      -Wno-variadic-macro-arguments-omitted
      -Wno-gnu-zero-variadic-macro-arguments)
    set_target_properties(isocline PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Upstream also defines an "isocline_shared" target that we don't
  # use. Two problems if we leave it as-is:
  #   1. It gets pulled into ALL, so `cmake --build .` (and
  #      `npm run build`) compiles it for nothing.
  #   2. It doesn't inherit our compile-option overrides above, so
  #      the same -Werror / variadic-macro warnings break the build
  #      on the shared target even though we never link it.
  # Fix both by applying the same warning suppressions and flipping
  # EXCLUDE_FROM_ALL.
  if(TARGET isocline_shared)
    target_compile_options(isocline_shared PRIVATE
      -Wno-error
      -Wno-variadic-macro-arguments-omitted
      -Wno-gnu-zero-variadic-macro-arguments)
    set_target_properties(isocline_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
  endif()
  if(NOT TARGET Isocline::Isocline)
    add_library(Isocline::Isocline INTERFACE IMPORTED GLOBAL)
    set_target_properties(Isocline::Isocline PROPERTIES
      INTERFACE_LINK_LIBRARIES      isocline
      INTERFACE_INCLUDE_DIRECTORIES "${isocline_SOURCE_DIR}/include"
    )
  endif()

  set(Isocline_INCLUDE_DIRS ${isocline_SOURCE_DIR}/include)
  set(Isocline_LIBRARIES    isocline)
  set(Isocline_FOUND TRUE)

  message(STATUS "FindIsocline: Built isocline from source")
endif()
