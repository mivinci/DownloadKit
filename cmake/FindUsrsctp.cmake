# FindUsrsctp.cmake - Find usrsctp (user-space SCTP) library
#
# This module defines:
#   Usrsctp_FOUND        - True if usrsctp was found
#   Usrsctp_INCLUDE_DIRS - Include directories for usrsctp
#   Usrsctp_LIBRARIES    - Libraries to link against
#
# And the imported target:
#   Usrsctp::Usrsctp     - The usrsctp library
#
# If usrsctp is not found on the system, it will be fetched and built
# from source via FetchContent.

# Prevent duplicate find
if(Usrsctp_FOUND)
  return()
endif()

include(FindPackageHandleStandardArgs)

# ---- Try system-installed usrsctp first ----
find_path(Usrsctp_INCLUDE_DIR
  NAMES usrsctp.h
  PATH_SUFFIXES usrsctp
  PATHS
    /usr/include
    /usr/local/include
    /opt/local/include
    /opt/homebrew/include
)

find_library(Usrsctp_LIBRARY
  NAMES usrsctp
  PATHS
    /usr/lib
    /usr/local/lib
    /opt/local/lib
    /opt/homebrew/lib
)

if(Usrsctp_INCLUDE_DIR AND Usrsctp_LIBRARY)
  # ---- Found on system ----
  find_package_handle_standard_args(Usrsctp
    REQUIRED_VARS Usrsctp_LIBRARY Usrsctp_INCLUDE_DIR
  )

  set(Usrsctp_INCLUDE_DIRS ${Usrsctp_INCLUDE_DIR})
  set(Usrsctp_LIBRARIES    ${Usrsctp_LIBRARY})

  if(NOT TARGET Usrsctp::Usrsctp)
    add_library(Usrsctp::Usrsctp UNKNOWN IMPORTED)
    set_target_properties(Usrsctp::Usrsctp PROPERTIES
      IMPORTED_LOCATION             "${Usrsctp_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${Usrsctp_INCLUDE_DIR}"
    )
  endif()

  mark_as_advanced(Usrsctp_INCLUDE_DIR Usrsctp_LIBRARY)
  message(STATUS "FindUsrsctp: Found system usrsctp: ${Usrsctp_LIBRARY}")
else()
  # ---- Fallback: fetch and build from source ----
  message(STATUS "FindUsrsctp: System usrsctp not found, fetching from source")

  include(FetchContent)
  FetchContent_Declare(
    usrsctp
    GIT_REPOSITORY https://github.com/sctplab/usrsctp.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  set(sctp_build_shared_lib OFF CACHE BOOL "" FORCE)
  set(sctp_build_programs   OFF CACHE BOOL "" FORCE)
  set(sctp_build_fuzzer     OFF CACHE BOOL "" FORCE)
  set(sctp_inet             OFF CACHE BOOL "" FORCE)
  set(sctp_inet6            OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(usrsctp)

  # Suppress warnings-as-errors inherited from parent project
  # and enable PIC for linking into shared libraries
  if(TARGET usrsctp)
    target_compile_options(usrsctp PRIVATE -Wno-error)
    set_target_properties(usrsctp PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  # Create an imported interface target wrapping the lib
  if(NOT TARGET Usrsctp::Usrsctp)
    add_library(Usrsctp::Usrsctp INTERFACE IMPORTED GLOBAL)
    set_target_properties(Usrsctp::Usrsctp PROPERTIES
      INTERFACE_LINK_LIBRARIES usrsctp
      INTERFACE_INCLUDE_DIRECTORIES "${usrsctp_SOURCE_DIR}/usrsctplib"
    )
  endif()

  set(Usrsctp_INCLUDE_DIRS ${usrsctp_SOURCE_DIR}/usrsctplib)
  set(Usrsctp_LIBRARIES    usrsctp)
  set(Usrsctp_FOUND TRUE)

  message(STATUS "FindUsrsctp: Built usrsctp from source")
endif()
