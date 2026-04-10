file(GLOB_RECURSE XP2P_HEADERS "*.h")
file(GLOB_RECURSE XP2P_SOURCES "*.c")
add_library(xp2p SHARED ${XP2P_SOURCES} ${XP2P_HEADERS})

target_link_libraries(xp2p PUBLIC xbase xnet)

message(STATUS "xp2p: P2P module enabled (ICE + STUN + TURN + SDP)")

if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XP2P_TEST_SOURCES "*_test.cpp")
  if(XP2P_TEST_SOURCES)
    add_executable(xp2p_test ${XP2P_TEST_SOURCES})
    target_link_libraries(xp2p_test PRIVATE xp2p GTest::gtest GTest::gtest_main)
    add_test(NAME xp2p_test COMMAND xp2p_test)
  endif()
endif()
