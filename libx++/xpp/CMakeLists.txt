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
file(GLOB_RECURSE XPP_BENCH_SOURCES "*_bench.cpp")
list(REMOVE_ITEM XPP_SOURCES ${XPP_TEST_SOURCES})
list(REMOVE_ITEM XPP_SOURCES ${XPP_BENCH_SOURCES})
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

# Optional: C++20 coroutine test target
# This is separate from the main test binary to allow C++11 and C++20
# binaries to coexist. When C++20 is available, we build both.
file(GLOB_RECURSE XPP_CORO_TEST_SOURCES "*_coro_test.cpp")
if(XPP_CORO_TEST_SOURCES AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  # Only attempt C++20 coroutine tests on compilers that support them
  add_executable(x++_coro_test ${XPP_CORO_TEST_SOURCES})
  set_target_properties(x++_coro_test PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  target_link_libraries(x++_coro_test PRIVATE x++ GTest::gtest_main)
  add_test(NAME x++_coro_test COMMAND x++_coro_test)
endif()

if(X_BUILD_BENCHMARKS AND TARGET benchmark::benchmark_main)
  foreach(src ${XPP_BENCH_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(x++_${name} ${src})
    target_link_libraries(x++_${name} PRIVATE x++ benchmark::benchmark_main)
    target_compile_options(x++_${name} PRIVATE -Wno-error)
    set_target_properties(x++_${name} PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
    )
  endforeach()
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
