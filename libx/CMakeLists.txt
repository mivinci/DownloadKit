# libx — C foundation libraries for moo
#
# Self-contained: includes its own bench and examples programs. Can be
# consumed as a sub-project; the parent project supplies the MOO_BUILD_*
# toggles.

add_subdirectory(x/base)
add_subdirectory(x/log)
add_subdirectory(x/buf)
add_subdirectory(x/net)
add_subdirectory(x/crypto)
add_subdirectory(x/http)
add_subdirectory(x/p2p)
add_subdirectory(x/fer)
add_subdirectory(x/agent)
add_subdirectory(x/js)
add_subdirectory(x/line)
add_subdirectory(x/tui)

if(MOO_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(MOO_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
