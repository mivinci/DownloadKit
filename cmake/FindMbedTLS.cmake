# FindMbedTLS.cmake - Find mbedTLS library
#
# This module defines:
#   MbedTLS_FOUND        - True if mbedTLS was found
#   MbedTLS_INCLUDE_DIRS - Include directories for mbedTLS
#   MbedTLS_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   MbedTLS::MbedTLS     - The mbedTLS library (includes mbedtls, mbedcrypto, mbedx509)

include(FindPackageHandleStandardArgs)

find_path(MbedTLS_INCLUDE_DIR
  NAMES mbedtls/ssl.h
  HINTS ${MBEDTLS_ROOT_DIR}/include
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(MbedTLS_LIBRARY
  NAMES mbedtls
  HINTS ${MBEDTLS_ROOT_DIR}/lib
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

find_library(MbedCrypto_LIBRARY
  NAMES mbedcrypto
  HINTS ${MBEDTLS_ROOT_DIR}/lib
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

find_library(MbedX509_LIBRARY
  NAMES mbedx509
  HINTS ${MBEDTLS_ROOT_DIR}/lib
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

find_package_handle_standard_args(MbedTLS
  REQUIRED_VARS MbedTLS_LIBRARY MbedCrypto_LIBRARY MbedX509_LIBRARY MbedTLS_INCLUDE_DIR
)

if(MbedTLS_FOUND)
  set(MbedTLS_INCLUDE_DIRS ${MbedTLS_INCLUDE_DIR})
  set(MbedTLS_LIBRARIES    ${MbedTLS_LIBRARY} ${MbedX509_LIBRARY} ${MbedCrypto_LIBRARY})

  if(NOT TARGET MbedTLS::MbedTLS)
    add_library(MbedTLS::MbedTLS INTERFACE IMPORTED)
    set_target_properties(MbedTLS::MbedTLS PROPERTIES
      INTERFACE_LINK_LIBRARIES      "${MbedTLS_LIBRARY};${MbedX509_LIBRARY};${MbedCrypto_LIBRARY}"
    )
  endif()
endif()

mark_as_advanced(MbedTLS_INCLUDE_DIR MbedTLS_LIBRARY MbedCrypto_LIBRARY MbedX509_LIBRARY)
