file(GLOB_RECURSE XFER_HEADERS "*.h")
file(GLOB_RECURSE XFER_SOURCES "*.c")

add_library(xfer SHARED ${XFER_SOURCES} ${XFER_HEADERS})

target_link_libraries(xfer PUBLIC xp2p xbase)

if(XK_ENABLE_DEBUG)
  target_compile_definitions(xfer PRIVATE XK_ENABLE_DEBUG)
endif()

message(STATUS "xfer: P2P file transfer module enabled")

# ── Tests ──────────────────────────────────────────────────────────────
if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XFER_TEST_SOURCES "*_test.cpp")
  if(XFER_TEST_SOURCES)
    add_executable(xfer_test ${XFER_TEST_SOURCES})
    target_link_libraries(xfer_test PRIVATE xfer xp2p xbase
                          GTest::gtest GTest::gtest_main)
    add_test(NAME xfer_test COMMAND xfer_test)
  endif()
endif()
