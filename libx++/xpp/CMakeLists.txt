# libxpp — C++ RAII Wrappers for libx
#
# Thin C++ wrappers that RAII-ify the C API in libx/.

# ── Sub-project options (decoupled from top-level MOO_* switches) ──
# Defaults inherit from the libx X_BUILD_TESTS switch when invoked from
# the root build, but can be set independently when libxpp is consumed
# standalone.
option(XPP_BUILD_TESTS "Build libxpp tests" ${X_BUILD_TESTS})

# Compile-only smoke target that proves the public headers are
# C++11-clean in strict (non-GNU-extension) mode. Off by default so a
# default build doesn't pay the small extra compile; CI flips it on.
option(XPP_CXX11_GUARD "Build the libx++ C++11 strict-mode compile guard" OFF)

file(GLOB XPP_SOURCES "*.cpp")
file(GLOB XPP_TEST_SOURCES "*_test.cpp")
list(REMOVE_ITEM XPP_SOURCES ${XPP_TEST_SOURCES})
list(REMOVE_ITEM XPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cxx11_guard.cpp")

add_library(x++ ${XPP_SOURCES})
add_library(xpp ALIAS x++)  # convenience alias for downstream consumers
target_compile_features(x++ PUBLIC cxx_std_11)
target_link_libraries(x++ PUBLIC xbase)
target_include_directories(x++ PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/libx
  ${CMAKE_SOURCE_DIR}/libx++
)

if(XPP_BUILD_TESTS)
  add_executable(x++_test ${XPP_TEST_SOURCES})
  target_link_libraries(x++_test PRIVATE x++ GTest::gtest_main)
  add_test(NAME x++_test COMMAND x++_test)
endif()

if(XPP_CXX11_GUARD)
  # Compile cxx11_guard.cpp on its own, in strict C++11 mode, with no
  # GNU extensions. Linking against x++ would re-import xbase's
  # transitive feature requirements; keep the guard self-contained
  # (header-only consumption) by dropping that link and its features
  # propagation. The guard is a compile-only check — there is no
  # runtime entry point.
  add_library(x++_cxx11_guard STATIC cxx11_guard.cpp)
  target_include_directories(x++_cxx11_guard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/libx
    ${CMAKE_SOURCE_DIR}/libx++
  )
  set_target_properties(x++_cxx11_guard PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
endif()
