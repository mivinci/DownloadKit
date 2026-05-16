file(GLOB_RECURSE XFER_HEADERS "*.h")
file(GLOB_RECURSE XFER_SOURCES "*.c")

add_library(xfer ${X_LIB_TYPE} ${XFER_SOURCES} ${XFER_HEADERS})

find_package(Cjson REQUIRED)

target_link_libraries(xfer PUBLIC xp2p xhttp xnet xcrypto xbase PRIVATE Cjson::Cjson)

message(STATUS "xfer: P2P file transfer module enabled")

# ── Tests ──────────────────────────────────────────────────────────────
option(XFER_BUILD_TESTS "Build xfer tests" ${X_BUILD_TESTS})
if(XFER_BUILD_TESTS)
  file(GLOB_RECURSE XFER_TEST_SOURCES "*_test.cpp")
  if(XFER_TEST_SOURCES)
    add_executable(xfer_test ${XFER_TEST_SOURCES})
    target_link_libraries(xfer_test PRIVATE xfer xp2p xbase
                          GTest::gtest_main)
    add_test(NAME xfer_test COMMAND xfer_test)
  endif()
endif()
