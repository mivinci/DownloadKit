file(GLOB_RECURSE XCRYPTO_HEADERS "*.h")

# Start with no sources; add conditionally below
set(XCRYPTO_SOURCES "")

add_library(xcrypto SHARED ${XCRYPTO_HEADERS})

target_link_libraries(xcrypto PUBLIC xbase)

# ── SHA-1 backend selection (reuses XK_TLS_BACKEND) ───────────────────
if(XK_TLS_BACKEND STREQUAL "openssl")
  find_package(OpenSSL REQUIRED)
  target_sources(xcrypto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sha1_openssl.c)
  target_link_libraries(xcrypto PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "xcrypto: SHA-1 backend = OpenSSL")
elseif(XK_TLS_BACKEND STREQUAL "mbedtls")
  find_package(MbedTLS REQUIRED)
  target_sources(xcrypto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sha1_mbedtls.c)
  target_include_directories(xcrypto BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
  target_link_libraries(xcrypto PRIVATE MbedTLS::MbedTLS)
  message(STATUS "xcrypto: SHA-1 backend = mbedTLS")
elseif(XK_TLS_BACKEND STREQUAL "auto")
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_sources(xcrypto PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/sha1_openssl.c)
    target_link_libraries(xcrypto PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "xcrypto: SHA-1 backend = OpenSSL (auto-detected)")
  else()
    find_package(MbedTLS QUIET)
    if(MbedTLS_FOUND)
      target_sources(xcrypto PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sha1_mbedtls.c)
      target_include_directories(xcrypto BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
      target_link_libraries(xcrypto PRIVATE MbedTLS::MbedTLS)
      message(STATUS "xcrypto: SHA-1 backend = mbedTLS (auto-detected)")
    else()
      target_sources(xcrypto PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sha1_builtin.c)
      message(STATUS "xcrypto: SHA-1 backend = builtin (pure C)")
    endif()
  endif()
else()
  target_sources(xcrypto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sha1_builtin.c)
  message(STATUS "xcrypto: SHA-1 backend = builtin (pure C)")
endif()

message(STATUS "xcrypto: crypto module enabled (SHA-1)")

# ── Tests ──────────────────────────────────────────────────────────────
if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XCRYPTO_TEST_SOURCES "*_test.cpp")
  if(XCRYPTO_TEST_SOURCES)
    add_executable(xcrypto_test ${XCRYPTO_TEST_SOURCES})
    target_link_libraries(xcrypto_test PRIVATE xcrypto xbase
                          GTest::gtest GTest::gtest_main)
    add_test(NAME xcrypto_test COMMAND xcrypto_test)
  endif()
endif()
