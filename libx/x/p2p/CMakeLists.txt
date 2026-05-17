file(GLOB_RECURSE XP2P_HEADERS "*.h")
file(GLOB_RECURSE XP2P_SOURCES "*.c")

# ── Exclude DTLS/SCTP/DataChannel backend sources from default GLOB ────
# These are added conditionally below based on X_TLS_BACKEND settings.
list(FILTER XP2P_SOURCES EXCLUDE REGEX "dtls_openssl\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "dtls_mbedtls\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "dtls_transport\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "sctp_transport\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "datachannel\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "peer_connection\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "ice_crypto\\.c$")
list(FILTER XP2P_HEADERS EXCLUDE REGEX "ice_crypto\\.h$")

add_library(xp2p ${X_LIB_TYPE} ${XP2P_SOURCES} ${XP2P_HEADERS})

target_link_libraries(xp2p PUBLIC xbase xnet xcrypto)

message(STATUS "xp2p: P2P module enabled (ICE + STUN + TURN + SDP + DTLS + SCTP + DataChannel)")

# ── DTLS / SCTP / DataChannel support (always enabled) ────────────────
target_sources(xp2p PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/dtls_transport.c
  ${CMAKE_CURRENT_SOURCE_DIR}/sctp_transport.c
  ${CMAKE_CURRENT_SOURCE_DIR}/datachannel.c
  ${CMAKE_CURRENT_SOURCE_DIR}/peer_connection.c
)

target_compile_definitions(xp2p PUBLIC XP2P_HAS_DTLS)

# ── TLS backend selection (reuses X_TLS_BACKEND from xnet) ────────────
if(X_TLS_BACKEND STREQUAL "openssl")
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(xp2p PRIVATE XP2P_TLS_OPENSSL)
  target_sources(xp2p PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dtls_openssl.c)
  target_link_libraries(xp2p PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "xp2p: DTLS TLS backend = OpenSSL")
elseif(X_TLS_BACKEND STREQUAL "mbedtls")
  find_package(MbedTLS REQUIRED)
  set_target_properties(MbedTLS::MbedTLS PROPERTIES IMPORTED_NO_SYSTEM TRUE)
  target_compile_definitions(xp2p PRIVATE XP2P_TLS_MBEDTLS)
  target_sources(xp2p PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dtls_mbedtls.c)
  target_include_directories(xp2p BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
  target_link_libraries(xp2p PRIVATE MbedTLS::MbedTLS)
  message(STATUS "xp2p: DTLS TLS backend = mbedTLS")
elseif(X_TLS_BACKEND STREQUAL "auto")
  # Mirror xnet's auto-detection order
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_compile_definitions(xp2p PRIVATE XP2P_TLS_OPENSSL)
    target_sources(xp2p PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/dtls_openssl.c)
    target_link_libraries(xp2p PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "xp2p: DTLS TLS backend = OpenSSL (auto-detected)")
  else()
    find_package(MbedTLS QUIET)
    if(MbedTLS_FOUND)
      set_target_properties(MbedTLS::MbedTLS PROPERTIES IMPORTED_NO_SYSTEM TRUE)
      target_compile_definitions(xp2p PRIVATE XP2P_TLS_MBEDTLS)
      target_sources(xp2p PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/dtls_mbedtls.c)
      target_include_directories(xp2p BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
      target_link_libraries(xp2p PRIVATE MbedTLS::MbedTLS)
      message(STATUS "xp2p: DTLS TLS backend = mbedTLS (auto-detected)")
    else()
      message(FATAL_ERROR "xp2p: No TLS library found (need OpenSSL or mbedTLS).")
    endif()
  endif()
else()
  message(FATAL_ERROR "xp2p: X_TLS_BACKEND='${X_TLS_BACKEND}' has no TLS support.")
endif()

# ── usrsctp (user-space SCTP) ──────────────────────────────────────────
find_package(Usrsctp REQUIRED)
set_target_properties(Usrsctp::Usrsctp PROPERTIES IMPORTED_NO_SYSTEM TRUE)
target_link_libraries(xp2p PRIVATE Usrsctp::Usrsctp)
target_include_directories(xp2p PRIVATE ${Usrsctp_INCLUDE_DIRS})

# ── Tests ──────────────────────────────────────────────────────────────
option(XP2P_BUILD_TESTS "Build xp2p tests" ${X_BUILD_TESTS})
if(XP2P_BUILD_TESTS)
  file(GLOB_RECURSE XP2P_TEST_SOURCES "*_test.cpp")
  if(XP2P_TEST_SOURCES)
    add_executable(xp2p_test ${XP2P_TEST_SOURCES})
    target_link_libraries(xp2p_test PRIVATE xp2p GTest::gtest_main)
    # Link TLS library for tests
    if(X_TLS_BACKEND STREQUAL "openssl" OR (X_TLS_BACKEND STREQUAL "auto" AND OpenSSL_FOUND))
      target_link_libraries(xp2p_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    elseif(X_TLS_BACKEND STREQUAL "mbedtls" OR (X_TLS_BACKEND STREQUAL "auto" AND MbedTLS_FOUND))
      target_include_directories(xp2p_test BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
      target_link_libraries(xp2p_test PRIVATE MbedTLS::MbedTLS)
    endif()
    target_link_libraries(xp2p_test PRIVATE Usrsctp::Usrsctp)
    target_include_directories(xp2p_test PRIVATE ${Usrsctp_INCLUDE_DIRS})
    add_test(NAME xp2p_test COMMAND xp2p_test)
    # NOTE: several ICE tests intentionally wait for STUN/TURN retransmission
    # timeouts (~11s each). On slow CI runners the accumulated wall time can
    # approach the CTest limit, so we give the whole binary a generous cap.
    set_tests_properties(xp2p_test PROPERTIES TIMEOUT 300)
  endif()
endif()
