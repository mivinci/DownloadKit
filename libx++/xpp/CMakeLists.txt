# libx++ — Rust-like infrastructure in C++11
#
# Header-mostly value types, RAII wrappers, and a ref-counted family
# inspired by Rust's std prelude (Option, Result, Variant, NonNull,
# Own, Rc, Weak, Arc, ArcWeak, Error, panic). Pull this in standalone
# wherever C++11 is available. Domain-specific bindings (e.g. C-API
# wrappers around libx) live in separate projects on top of this one.

option(XPP_BUILD_TESTS "Build libx++ tests" ${X_BUILD_TESTS})

# Compile-only smoke target that proves the public headers are
# C++11-clean in strict (non-GNU-extension) mode. Off by default so a
# default build doesn't pay the small extra compile; CI flips it on.
option(XPP_CXX11_GUARD "Build the libx++ C++11 strict-mode compile guard" OFF)

file(GLOB_RECURSE XPP_SOURCES "*.cpp")
file(GLOB_RECURSE XPP_TEST_SOURCES "*_test.cpp")
list(REMOVE_ITEM XPP_SOURCES ${XPP_TEST_SOURCES})
list(REMOVE_ITEM XPP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cxx11_guard.cpp")

# The public surface (Option / Result / Variant / NonNull / Own / Rc
# / Weak / Arc / ArcWeak / Error) is header-only and instantiates in
# the consumer's TU. The .cpp files that land here are out-of-line
# bridges whose only job is to keep heavy / platform headers out of
# the public surface:
#   panic.cpp   - routes fatal messages through libx's xLog, so
#                 <xpp/panic.h> doesn't pull in <x/base/log.h>.
#   mutex.cpp / cond.cpp
#               - bridge xpp::Mutex / xpp::Condvar to libx's xMutex /
#                 xCond via opaque byte buffers, so <xpp/mutex.h> and
#                 <xpp/cond.h> don't transitively pull in
#                 <pthread.h> on POSIX or <windows.h> on Windows.
add_library(x++ ${XPP_SOURCES})
add_library(xpp ALIAS x++)  # convenience alias for downstream consumers
target_compile_features(x++ PUBLIC cxx_std_11)
target_link_libraries(x++ PUBLIC xbase)
target_include_directories(x++ PUBLIC
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
    ${CMAKE_SOURCE_DIR}/libx
    ${CMAKE_SOURCE_DIR}/libx++
  )
  set_target_properties(x++_cxx11_guard PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
endif()
