file(GLOB_RECURSE XTUI_HEADERS "*.h")
file(GLOB_RECURSE XTUI_SOURCES "*.c")
add_library(xtui ${MOO_LIB_TYPE} ${XTUI_SOURCES} ${XTUI_HEADERS})

# xtui only uses xbase for the XCAPI / XDEF_STRUCT macros (no link
# dependency at runtime), but we still link PUBLIC so downstream
# targets that include <xtui/md.h> pick up <xbase/base.h> correctly.
target_link_libraries(xtui PUBLIC xbase)

message(STATUS "xtui: terminal UI primitives enabled")

if(MOO_BUILD_TESTS)
  file(GLOB_RECURSE XTUI_TEST_SOURCES "*_test.cpp")
  if(XTUI_TEST_SOURCES)
    add_executable(xtui_test ${XTUI_TEST_SOURCES})
    target_link_libraries(xtui_test PRIVATE xtui GTest::gtest_main)
    add_test(NAME xtui_test COMMAND xtui_test)
  endif()
endif()
