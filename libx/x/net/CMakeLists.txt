file(GLOB_RECURSE XNET_HEADERS "*.h")
file(GLOB_RECURSE XNET_SOURCES "*.c")
list(FILTER XNET_HEADERS EXCLUDE REGEX "_test")
# Exclude TLS backend sources from default GLOB;
# they are added conditionally below
list(FILTER XNET_SOURCES EXCLUDE REGEX "tls_openssl")
list(FILTER XNET_SOURCES EXCLUDE REGEX "tls_mbedtls")
list(FILTER XNET_SOURCES EXCLUDE REGEX "transport_openssl")
list(FILTER XNET_SOURCES EXCLUDE REGEX "transport_mbedtls")
add_library(xnet ${X_LIB_TYPE} ${XNET_SOURCES} ${XNET_HEADERS})
target_link_libraries(xnet PUBLIC xbase xbuf)

# ── TLS backend detection (mirrors xhttp pattern) ─────────────────────
# Reuses the X_TLS_BACKEND cache variable set by xhttp or the user.
if(NOT DEFINED X_TLS_BACKEND)
  set(X_TLS_BACKEND "auto" CACHE STRING "TLS backend: auto, openssl, mbedtls, none")
endif()

if(X_TLS_BACKEND STREQUAL "none")
  message(STATUS "xnet: TLS support disabled by user")
elseif(X_TLS_BACKEND STREQUAL "openssl")
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(xnet PUBLIC X_HAS_OPENSSL)
  target_sources(xnet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tls_openssl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/transport_openssl.c)
  target_link_libraries(xnet PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "xnet: TLS backend = OpenSSL (forced)")
elseif(X_TLS_BACKEND STREQUAL "mbedtls")
  find_package(MbedTLS REQUIRED)
  set_target_properties(MbedTLS::MbedTLS PROPERTIES IMPORTED_NO_SYSTEM TRUE)
  target_compile_definitions(xnet PUBLIC X_HAS_MBEDTLS)
  target_sources(xnet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tls_mbedtls.c
    ${CMAKE_CURRENT_SOURCE_DIR}/transport_mbedtls.c)
  target_include_directories(xnet BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
  target_link_libraries(xnet PRIVATE MbedTLS::MbedTLS)
  message(STATUS "xnet: TLS backend = mbedTLS (forced)")
elseif(X_TLS_BACKEND STREQUAL "auto")
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_compile_definitions(xnet PUBLIC X_HAS_OPENSSL)
    target_sources(xnet PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tls_openssl.c
      ${CMAKE_CURRENT_SOURCE_DIR}/transport_openssl.c)
    target_link_libraries(xnet PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "xnet: TLS backend = OpenSSL (auto-detected)")
  else()
    find_package(MbedTLS QUIET)
    if(MbedTLS_FOUND)
      set_target_properties(MbedTLS::MbedTLS PROPERTIES IMPORTED_NO_SYSTEM TRUE)
      target_compile_definitions(xnet PUBLIC X_HAS_MBEDTLS)
      target_sources(xnet PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tls_mbedtls.c
        ${CMAKE_CURRENT_SOURCE_DIR}/transport_mbedtls.c)
      target_include_directories(xnet BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
      target_link_libraries(xnet PRIVATE MbedTLS::MbedTLS)
      message(STATUS "xnet: TLS backend = mbedTLS (auto-detected)")
    else()
      message(STATUS "xnet: No TLS library found, TLS support disabled")
    endif()
  endif()
else()
  message(WARNING "xnet: Unknown X_TLS_BACKEND '${X_TLS_BACKEND}', TLS support disabled")
endif()

option(XNET_BUILD_TESTS "Build xnet tests" ${X_BUILD_TESTS})
if(XNET_BUILD_TESTS)
  file(GLOB_RECURSE XNET_TEST_SOURCES "*_test.cpp")
  if(XNET_TEST_SOURCES)
    add_executable(xnet_test ${XNET_TEST_SOURCES})
    target_link_libraries(xnet_test PRIVATE xnet GTest::gtest_main)
    # Link TLS library for integration tests
    if(TARGET OpenSSL::SSL)
      target_link_libraries(xnet_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    elseif(TARGET MbedTLS::MbedTLS)
      target_include_directories(xnet_test BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
      target_link_libraries(xnet_test PRIVATE MbedTLS::MbedTLS)
    endif()
    add_test(NAME xnet_test COMMAND xnet_test)
  endif()
endif()
