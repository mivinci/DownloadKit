# FindLibcurl.cmake - Find libcurl library
#
# This module defines:
#   Libcurl_FOUND        - True if libcurl was found
#   Libcurl_INCLUDE_DIRS - Include directories for libcurl
#   Libcurl_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Libcurl::Libcurl     - The libcurl library
#
# If libcurl is not found on the system, it will be fetched and built
# from source via FetchContent (with OpenSSL TLS and HTTP/2 support).

# Prevent duplicate find
if(Libcurl_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed libcurl first ----
find_path(Libcurl_INCLUDE_DIR
  NAMES curl/curl.h
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Libcurl_LIBRARY
  NAMES curl
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Libcurl_INCLUDE_DIR AND Libcurl_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Libcurl
    REQUIRED_VARS Libcurl_LIBRARY Libcurl_INCLUDE_DIR
  )

  set(Libcurl_INCLUDE_DIRS ${Libcurl_INCLUDE_DIR})
  set(Libcurl_LIBRARIES    ${Libcurl_LIBRARY})

  if(NOT TARGET Libcurl::Libcurl)
    add_library(Libcurl::Libcurl UNKNOWN IMPORTED)
    set_target_properties(Libcurl::Libcurl PROPERTIES
      IMPORTED_LOCATION             "${Libcurl_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Libcurl_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Libcurl_INCLUDE_DIR Libcurl_LIBRARY)
  message(STATUS "FindLibcurl: Found system libcurl: ${Libcurl_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindLibcurl: System libcurl not found, fetching from source")

  # Ensure OpenSSL is available (curl needs it for TLS)
  find_package(OpenSSL QUIET)
  if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR
      "FindLibcurl: Building libcurl from source requires OpenSSL.\n"
      "  Install it: apt install libssl-dev (Linux) / brew install openssl (macOS)")
  endif()

  # Ensure nghttp2 is available (curl needs it for HTTP/2).
  # Our FindNghttp2.cmake will fetch it from source if not installed.
  find_package(Nghttp2 REQUIRED)

  # If nghttp2 was built from source via FetchContent, curl's internal
  # find_package(NGHTTP2) won't find it. We set the hint variables and
  # create the target that curl expects.
  if(NOT TARGET NGHTTP2::nghttp2)
    if(TARGET nghttp2_static)
      # FetchContent-built nghttp2: create the alias target curl expects
      add_library(NGHTTP2::nghttp2 ALIAS nghttp2_static)
      set(NGHTTP2_INCLUDE_DIR "${Nghttp2_INCLUDE_DIRS}" CACHE PATH "" FORCE)
      set(NGHTTP2_LIBRARY "nghttp2_static" CACHE STRING "" FORCE)
    elseif(Nghttp2_LIBRARY)
      # System-installed nghttp2: create the imported target curl expects
      add_library(NGHTTP2::nghttp2 UNKNOWN IMPORTED)
      set_target_properties(NGHTTP2::nghttp2 PROPERTIES
        IMPORTED_LOCATION "${Nghttp2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Nghttp2_INCLUDE_DIRS}"
      )
      set(NGHTTP2_INCLUDE_DIR "${Nghttp2_INCLUDE_DIRS}" CACHE PATH "" FORCE)
      set(NGHTTP2_LIBRARY "${Nghttp2_LIBRARY}" CACHE STRING "" FORCE)
    endif()
    set(NGHTTP2_FOUND TRUE CACHE BOOL "" FORCE)
  endif()

  include(FetchContent)
  xk_github_url(curl/curl curl-8_11_1 _curl_url)
  FetchContent_Declare(
    curl
    URL ${_curl_url}
  )

  # ── Core build options ──
  set(BUILD_CURL_EXE    OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
  set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

  # ── TLS: use OpenSSL ──
  set(CURL_USE_OPENSSL  ON  CACHE BOOL "" FORCE)
  set(CURL_USE_MBEDTLS  OFF CACHE BOOL "" FORCE)
  set(CURL_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
  set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)

  # ── HTTP/2: use nghttp2 ──
  set(USE_NGHTTP2       ON  CACHE BOOL "" FORCE)

  # ── Disable unused protocols / features to speed up build ──
  set(CURL_DISABLE_LDAP    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAPS   ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_TELNET  ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_DICT    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_TFTP    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_POP3    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_IMAP    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_SMTP    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_GOPHER  ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_RTSP    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_MQTT    ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_SMB     ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_FTP     ON CACHE BOOL "" FORCE)
  set(CURL_DISABLE_FILE    ON CACHE BOOL "" FORCE)

  # ── Disable optional dependencies we don't need ──
  set(CURL_ZLIB        OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI      OFF CACHE BOOL "" FORCE)
  set(CURL_ZSTD        OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
  set(CURL_USE_LIBPSL  OFF CACHE BOOL "" FORCE)
  set(USE_LIBIDN2      OFF CACHE BOOL "" FORCE)

  FetchContent_MakeAvailable(curl)

  # Suppress warnings-as-errors inherited from parent project
  # and enable PIC for linking into shared libraries
  if(TARGET libcurl_static)
    target_compile_options(libcurl_static PRIVATE -Wno-error)
    set_target_properties(libcurl_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create the Libcurl::Libcurl imported target wrapping the static lib
  if(NOT TARGET Libcurl::Libcurl)
    add_library(Libcurl::Libcurl INTERFACE IMPORTED GLOBAL)
    set_target_properties(Libcurl::Libcurl PROPERTIES
      INTERFACE_LINK_LIBRARIES    libcurl_static
      INTERFACE_INCLUDE_DIRECTORIES "${curl_SOURCE_DIR}/include"
    )
  endif()

  set(Libcurl_INCLUDE_DIRS "${curl_SOURCE_DIR}/include")
  set(Libcurl_LIBRARIES    libcurl_static)
  set(Libcurl_FOUND TRUE)

  message(STATUS "FindLibcurl: Built libcurl from source (OpenSSL + HTTP/2)")
endif()

mark_as_advanced(Libcurl_INCLUDE_DIR Libcurl_LIBRARY)
