# FindNgtcp2.cmake - Find ngtcp2 (QUIC transport)
#
# This module defines:
#   Ngtcp2_FOUND        - True if ngtcp2 was found
#   Ngtcp2_INCLUDE_DIRS - Include directories
#   Ngtcp2_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Ngtcp2::Ngtcp2      - The ngtcp2 library
#
# For HTTP/3 support, ngtcp2_crypto_openssl (or boringssl/wolfssl) is also
# required.  This module looks for the crypto helper library matching the
# active TLS backend.

if(Ngtcp2_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

find_path(Ngtcp2_INCLUDE_DIR
  NAMES ngtcp2/ngtcp2.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
    /opt/homebrew/opt/libngtcp2/include
)

find_library(Ngtcp2_LIBRARY
  NAMES ngtcp2
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
    /opt/homebrew/opt/libngtcp2/lib
)

# Also need the crypto helper (ngtcp2_crypto_openssl)
find_library(Ngtcp2_Crypto_LIBRARY
  NAMES ngtcp2_crypto_ossl ngtcp2_crypto_openssl
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
    /opt/homebrew/opt/libngtcp2/lib
)

find_package_handle_standard_args(Ngtcp2
  REQUIRED_VARS Ngtcp2_LIBRARY Ngtcp2_INCLUDE_DIR Ngtcp2_Crypto_LIBRARY
)

if(Ngtcp2_FOUND)
  set(Ngtcp2_INCLUDE_DIRS ${Ngtcp2_INCLUDE_DIR})
  set(Ngtcp2_LIBRARIES    ${Ngtcp2_LIBRARY} ${Ngtcp2_Crypto_LIBRARY})

  if(NOT TARGET Ngtcp2::Ngtcp2)
    add_library(Ngtcp2::Ngtcp2 UNKNOWN IMPORTED)
    set_target_properties(Ngtcp2::Ngtcp2 PROPERTIES
      IMPORTED_LOCATION             "${Ngtcp2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Ngtcp2_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES      "${Ngtcp2_Crypto_LIBRARY}"
    )
  endif()

  message(STATUS "FindNgtcp2: Found ngtcp2 ${Ngtcp2_LIBRARY}")
else()
  message(STATUS "FindNgtcp2: ngtcp2 not found (HTTP/3 client disabled)")
endif()

mark_as_advanced(Ngtcp2_INCLUDE_DIR Ngtcp2_LIBRARY Ngtcp2_Crypto_LIBRARY)
