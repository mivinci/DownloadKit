# xai: LLM agent core built on top of xKit (xbase + xnet + xhttp).
#
# MVP source set: message helpers, tool registry, provider vtable
# dispatch, and the OpenAI-compatible provider. The agent / session
# loop lands in a follow-up batch; their headers already ship so
# downstream code can include them.

file(GLOB_RECURSE XAI_HEADERS "*.h")
file(GLOB_RECURSE XAI_SOURCES "*.c")

add_library(xai ${XK_LIB_TYPE} ${XAI_SOURCES} ${XAI_HEADERS})

# cJSON is a PRIVATE dep: provider_openai.c builds the request body
# and parses the SSE delta stream with it, but none of xai's installed
# headers reference cJSON types.
find_package(Cjson REQUIRED)

target_link_libraries(xai
                      PUBLIC  xbase xnet xhttp
                      PRIVATE Cjson::Cjson)

message(STATUS "xai: LLM agent core enabled "
               "(message + tool + provider + openai)")

# ── Tests ──────────────────────────────────────────────────────────────
if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XAI_TEST_SOURCES "*_test.cpp")
  if(XAI_TEST_SOURCES)
    add_executable(xai_test ${XAI_TEST_SOURCES})
    target_link_libraries(xai_test PRIVATE xai xhttp xbase
                          GTest::gtest_main)
    # Tests reach into module-private headers (tool_private.h,
    # provider_private.h); those live next to the .c files, so the
    # current source dir must be on the include path.
    target_include_directories(xai_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME xai_test COMMAND xai_test)
  endif()
endif()
