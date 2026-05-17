# libx — C foundation libraries for moo
#
# Self-contained: includes its own bench and examples programs and owns
# all library-build toggles under the X_* namespace. Can be consumed as
# a sub-project; the parent project sees only MOO_* application toggles.

# ── Library-build toggles ─────────────────────────────────────────────
option(X_BUILD_STATIC     "Build libx as static libraries"           OFF)
option(X_BUILD_TESTS      "Build libx tests"                          ON)
option(X_BUILD_BENCHMARKS "Build libx benchmarks"                     ON)
option(X_BUILD_EXAMPLES   "Build libx example programs"              OFF)
set(X_TLS_BACKEND "auto" CACHE STRING
    "TLS backend: auto, openssl, mbedtls, none")
set(X_DEBUG_LEVEL 0 CACHE STRING
    "Debug logging level (0-3, higher means more verbose)")

if(X_BUILD_STATIC)
  set(X_LIB_TYPE STATIC)
else()
  set(X_LIB_TYPE SHARED)
endif()

# Pass debug level to libx C/C++ sources
add_compile_definitions(X_DEBUG_LEVEL=${X_DEBUG_LEVEL})

if(X_BUILD_TESTS)
  find_package(GTest REQUIRED)
  # Promote IMPORTED targets to GLOBAL so libx++ (a sibling subdir) can
  # link against them too. CMake CONFIG-mode find_package and our
  # FindGTest fallback both create directory-scoped IMPORTED targets by
  # default; promotion lets the targets cross subdirectory boundaries.
  # ALIAS targets (e.g. when CONFIG mode aliases GTest::gtest_main onto
  # an internal name) cannot be promoted directly — promote the aliased
  # target instead, or skip if already global.
  foreach(_tgt GTest::gtest GTest::gtest_main)
    if(TARGET ${_tgt})
      get_target_property(_aliased ${_tgt} ALIASED_TARGET)
      if(_aliased)
        set(_real ${_aliased})
      else()
        set(_real ${_tgt})
      endif()
      get_target_property(_is_global ${_real} IMPORTED_GLOBAL)
      if(NOT _is_global)
        set_target_properties(${_real} PROPERTIES IMPORTED_GLOBAL TRUE)
      endif()
    endif()
  endforeach()
endif()

if(X_BUILD_BENCHMARKS)
  find_package(GBenchmark REQUIRED)
  foreach(_tgt GBenchmark::benchmark GBenchmark::benchmark_main)
    if(TARGET ${_tgt})
      get_target_property(_aliased ${_tgt} ALIASED_TARGET)
      if(_aliased)
        set(_real ${_aliased})
      else()
        set(_real ${_tgt})
      endif()
      get_target_property(_is_global ${_real} IMPORTED_GLOBAL)
      if(NOT _is_global)
        set_target_properties(${_real} PROPERTIES IMPORTED_GLOBAL TRUE)
      endif()
    endif()
  endforeach()
endif()

# ── Modules ───────────────────────────────────────────────────────────
add_subdirectory(x/base)
add_subdirectory(x/log)
add_subdirectory(x/buf)
add_subdirectory(x/net)
add_subdirectory(x/crypto)
add_subdirectory(x/http)
add_subdirectory(x/p2p)
add_subdirectory(x/fer)
add_subdirectory(x/agent)
add_subdirectory(x/js)
add_subdirectory(x/line)
add_subdirectory(x/tui)

if(X_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(X_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
