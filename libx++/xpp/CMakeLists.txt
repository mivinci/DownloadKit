# libxpp — C++ RAII Wrappers for libx
#
# Thin C++ wrappers that RAII-ify the C API in libx/.

# ── Sub-project options (decoupled from top-level MOO_* switches) ──
# Defaults inherit from the top-level switches when invoked from the root
# build, but can be set independently when libxpp is consumed standalone.
option(XPP_BUILD_TESTS "Build libxpp tests" ${MOO_BUILD_TESTS})

file(GLOB XPP_SOURCES "*.cpp")
file(GLOB XPP_TEST_SOURCES "*_test.cpp")
list(REMOVE_ITEM XPP_SOURCES ${XPP_TEST_SOURCES})

add_library(xpp ${XPP_SOURCES})
target_compile_features(xpp PUBLIC cxx_std_14)
target_link_libraries(xpp PUBLIC xbase)
target_include_directories(xpp PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/libx
  ${CMAKE_SOURCE_DIR}/libx++
)

if(XPP_BUILD_TESTS)
  add_executable(xpp_test ${XPP_TEST_SOURCES})
  target_link_libraries(xpp_test PRIVATE xpp GTest::gtest_main)
  add_test(NAME xpp_test COMMAND xpp_test)
endif()
