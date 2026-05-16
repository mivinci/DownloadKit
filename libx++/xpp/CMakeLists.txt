# libxpp — C++ RAII Wrappers for libx
#
# Thin C++ wrappers that RAII-ify the C API in libx/.

# ── Sub-project options (decoupled from top-level MOO_* switches) ──
# Defaults inherit from the libx X_BUILD_TESTS switch when invoked from
# the root build, but can be set independently when libxpp is consumed
# standalone.
option(XPP_BUILD_TESTS "Build libxpp tests" ${X_BUILD_TESTS})

file(GLOB XPP_SOURCES "*.cpp")
file(GLOB XPP_TEST_SOURCES "*_test.cpp")
list(REMOVE_ITEM XPP_SOURCES ${XPP_TEST_SOURCES})

add_library(x++ ${XPP_SOURCES})
target_compile_features(x++ PUBLIC cxx_std_14)
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
