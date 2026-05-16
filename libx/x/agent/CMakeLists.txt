# xagent: LLM agent core built on top of moo (xbase + xnet + xhttp).
#
# MVP source set: message helpers, tool registry, provider vtable
# dispatch, and the OpenAI-compatible provider. The agent / session
# loop lands in a follow-up batch; their headers already ship so
# downstream code can include them.

file(GLOB_RECURSE XAGENT_HEADERS "*.h")
file(GLOB_RECURSE XAGENT_SOURCES "*.c")

add_library(xagent ${X_LIB_TYPE} ${XAGENT_SOURCES} ${XAGENT_HEADERS})

# cJSON is a PRIVATE dep: provider_openai.c builds the request body
# and parses the SSE delta stream with it, but none of xagent's installed
# headers reference cJSON types.
find_package(Cjson REQUIRED)

target_link_libraries(xagent
                      PUBLIC  xbase xnet xhttp
                      PRIVATE Cjson::Cjson)

message(STATUS "xagent: LLM agent core enabled "
               "(message + tool + provider + openai)")

# ── Tests ──────────────────────────────────────────────────────────────
option(XAGENT_BUILD_TESTS "Build xagent tests" ${X_BUILD_TESTS})
if(XAGENT_BUILD_TESTS)
  file(GLOB_RECURSE XAGENT_TEST_SOURCES "*_test.cpp")
  if(XAGENT_TEST_SOURCES)
    add_executable(xagent_test ${XAGENT_TEST_SOURCES})
    target_link_libraries(xagent_test PRIVATE xagent xhttp xbase
                          GTest::gtest_main)
    # Tests reach into module-private headers (tool_private.h,
    # provider_private.h); those live next to the .c files, so the
    # current source dir must be on the include path.
    target_include_directories(xagent_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME xagent_test COMMAND xagent_test)
  endif()
endif()
