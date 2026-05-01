# xjs: JavaScript scripting engine built on QuickJS.
#
# Provides a lightweight JS VM that can be embedded in xKit
# applications.  Designed to pair with xai so that tool/agent
# logic can be written in JavaScript instead of C.

file(GLOB_RECURSE XJS_HEADERS "*.h")
file(GLOB_RECURSE XJS_SOURCES "*.c")

add_library(xjs ${XK_LIB_TYPE} ${XJS_SOURCES} ${XJS_HEADERS})

# QuickJS is a PRIVATE dep: xjs implementation wraps the QuickJS
# API internally, but none of xjs's installed headers reference
# QuickJS types.
find_package(QuickJS REQUIRED)

target_link_libraries(xjs
                      PUBLIC  xbase
                      PRIVATE QuickJS::QuickJS)

message(STATUS "xjs: JavaScript scripting engine enabled (QuickJS)")

# ── Tests ──────────────────────────────────────────────────────────────
if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XJS_TEST_SOURCES "*_test.cpp")
  if(XJS_TEST_SOURCES)
    add_executable(xjs_test ${XJS_TEST_SOURCES})
    # Tests reach into module-private headers (js_private.h) which
    # would normally include quickjs.h.  That header's source
    # directory unfortunately contains a bare file named `version`
    # that shadows libc++'s `<version>` standard header, so we
    # build the test translation units with XJS_TEST_NO_QUICKJS to
    # replace the quickjs.h include with minimal forward declarations
    # (see js_private.h).  Symbols provided by QuickJS are still
    # reachable through the xjs shared library at link time.
    target_compile_definitions(xjs_test PRIVATE XJS_TEST_NO_QUICKJS)
    target_link_libraries(xjs_test PRIVATE xjs xbase
                          GTest::gtest_main)
    # Tests reach into module-private headers; those live next to
    # the .c files, so the current source dir must be on the include
    # path.
    target_include_directories(xjs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME xjs_test COMMAND xjs_test)
  endif()
endif()
