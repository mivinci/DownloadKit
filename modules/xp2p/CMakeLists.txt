file(GLOB_RECURSE XP2P_HEADERS "*.h")
file(GLOB_RECURSE XP2P_SOURCES "*.c")

# ── Exclude DTLS/SCTP/DataChannel backend sources from default GLOB ────
# These are added conditionally below based on XP2P_ENABLE_DTLS and
# XK_TLS_BACKEND settings.
list(FILTER XP2P_SOURCES EXCLUDE REGEX "dtls_openssl\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "dtls_mbedtls\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "dtls_transport\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "sctp_transport\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "datachannel\\.c$")
list(FILTER XP2P_SOURCES EXCLUDE REGEX "peer_connection\\.c$")

add_library(xp2p ${XK_LIB_TYPE} ${XP2P_SOURCES} ${XP2P_HEADERS})

target_link_libraries(xp2p PUBLIC xbase xnet)

message(STATUS "xp2p: P2P module enabled (ICE + STUN + TURN + SDP)")

# ── DTLS / SCTP / DataChannel support ─────────────────────────────────
option(XP2P_ENABLE_DTLS "Enable DTLS + SCTP + DataChannel for WebRTC interop" ON)

if(XP2P_ENABLE_DTLS)
  # ── TLS backend selection (reuses XK_TLS_BACKEND from xnet) ────────
  # Add DTLS/SCTP/DataChannel/PeerConnection common sources
  target_sources(xp2p PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/dtls_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sctp_transport.c
    ${CMAKE_CURRENT_SOURCE_DIR}/datachannel.c
    ${CMAKE_CURRENT_SOURCE_DIR}/peer_connection.c
  )

  target_compile_definitions(xp2p PUBLIC XP2P_HAS_DTLS)

  if(XK_TLS_BACKEND STREQUAL "openssl")
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(xp2p PRIVATE XP2P_TLS_OPENSSL)
    target_sources(xp2p PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/dtls_openssl.c)
    target_link_libraries(xp2p PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "xp2p: DTLS TLS backend = OpenSSL")
  elseif(XK_TLS_BACKEND STREQUAL "mbedtls")
    find_package(MbedTLS REQUIRED)
    set_target_properties(MbedTLS::MbedTLS PROPERTIES IMPORTED_NO_SYSTEM TRUE)
    target_compile_definitions(xp2p PRIVATE XP2P_TLS_MBEDTLS)
    target_sources(xp2p PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/dtls_mbedtls.c)
    target_include_directories(xp2p BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
    target_link_libraries(xp2p PRIVATE MbedTLS::MbedTLS)
    message(STATUS "xp2p: DTLS TLS backend = mbedTLS")
  elseif(XK_TLS_BACKEND STREQUAL "auto")
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
        message(FATAL_ERROR "xp2p: DTLS enabled but no TLS library found.")
      endif()
    endif()
  else()
    message(FATAL_ERROR "xp2p: DTLS enabled but XK_TLS_BACKEND='${XK_TLS_BACKEND}' has no TLS support.")
  endif()

  # ── usrsctp (user-space SCTP) ────────────────────────────────────────
  find_package(Usrsctp REQUIRED)
  set_target_properties(Usrsctp::Usrsctp PROPERTIES IMPORTED_NO_SYSTEM TRUE)
  target_link_libraries(xp2p PRIVATE Usrsctp::Usrsctp)
  target_include_directories(xp2p PRIVATE ${Usrsctp_INCLUDE_DIRS})

  message(STATUS "xp2p: DTLS + SCTP + DataChannel enabled")
else()
  message(STATUS "xp2p: DTLS disabled, pure ICE mode only")
endif()

# ── Tests ──────────────────────────────────────────────────────────────
if(XK_BUILD_TESTS)
  file(GLOB_RECURSE XP2P_TEST_SOURCES "*_test.cpp")
  if(XP2P_TEST_SOURCES)
    add_executable(xp2p_test ${XP2P_TEST_SOURCES})
    target_link_libraries(xp2p_test PRIVATE xp2p GTest::gtest_main)
    # Link TLS library for tests that need DTLS
    if(XP2P_ENABLE_DTLS)
      if(XK_TLS_BACKEND STREQUAL "openssl" OR (XK_TLS_BACKEND STREQUAL "auto" AND OpenSSL_FOUND))
        target_link_libraries(xp2p_test PRIVATE OpenSSL::SSL OpenSSL::Crypto)
      elseif(XK_TLS_BACKEND STREQUAL "mbedtls" OR (XK_TLS_BACKEND STREQUAL "auto" AND MbedTLS_FOUND))
        target_include_directories(xp2p_test BEFORE PRIVATE ${MbedTLS_INCLUDE_DIRS})
        target_link_libraries(xp2p_test PRIVATE MbedTLS::MbedTLS)
      endif()
      target_link_libraries(xp2p_test PRIVATE Usrsctp::Usrsctp)
      target_include_directories(xp2p_test PRIVATE ${Usrsctp_INCLUDE_DIRS})
    endif()
    add_test(NAME xp2p_test COMMAND xp2p_test)
  endif()
endif()
