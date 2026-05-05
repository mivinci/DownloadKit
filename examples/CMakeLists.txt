add_executable(llm_sse llm_sse.cpp)
target_link_libraries(llm_sse PRIVATE xhttp xbase)

add_executable(ws_echo_server ws_echo_server.cpp)
target_link_libraries(ws_echo_server PRIVATE xhttp xbase)

add_executable(ws_client ws_client.cpp)
target_link_libraries(ws_client PRIVATE xhttp xbase)

add_executable(ice_echo ice_echo.c)
target_link_libraries(ice_echo PRIVATE xp2p xbase)

add_executable(pc_echo pc_echo.c)
target_link_libraries(pc_echo PRIVATE xp2p xbase)

add_executable(pc_server pc_server.cpp)
target_link_libraries(pc_server PRIVATE xhttp xbase)

add_executable(pc_client pc_client.cpp)
target_link_libraries(pc_client PRIVATE xp2p xhttp xbase)

add_executable(xfer_echo xfer_echo.c)
target_link_libraries(xfer_echo PRIVATE xfer xbase)

add_executable(xfer_send xfer_send.c)
target_link_libraries(xfer_send PRIVATE xfer xbase)

add_executable(xfer_recv xfer_recv.c)
target_link_libraries(xfer_recv PRIVATE xfer xbase)

add_executable(xfer_signal xfer_signal.c)
target_link_libraries(xfer_signal PRIVATE xfer xhttp xbase)

add_executable(nat_probe nat_probe.c)
target_link_libraries(nat_probe PRIVATE xp2p xbase)

add_executable(ai_openai ai_openai.cpp)
target_link_libraries(ai_openai PRIVATE xagent xhttp xbase)
# Demo reaches into libs/xagent/provider_private.h for the internal
# dispatcher because session.c has not landed yet. Once session lands,
# this include path should be removed and the demo rewritten against
# xAgentCreate + xAgentSessionCreate.
target_include_directories(ai_openai PRIVATE
  ${CMAKE_SOURCE_DIR}/libs)

# The canonical end-to-end xagent REPL that used to live here has
# graduated to apps/cli/. Build it via the top-level project — it's
# no longer gated behind MOO_BUILD_EXAMPLES.

# ES-module demo:
# ES-module demo: show how to expose host-native functionality as
# an `import`-able module by combining native callbacks on the
# global object with a module loader that synthesises a small JS
# facade.  See the file header comment for the full rationale.
add_executable(xjs_native_module xjs_native_module.c)
target_link_libraries(xjs_native_module PRIVATE xjs)
target_include_directories(xjs_native_module PRIVATE
  ${CMAKE_SOURCE_DIR}/libs)

# Minimal skeleton for "AI TUI" style apps: multiplex xline's async
# line editor with an xbase event loop, with a periodic timer
# standing in for a streaming AI response. See the file header for
# the full walkthrough.
add_executable(xline_async xline_async.c)
target_link_libraries(xline_async PRIVATE xline xbase)
target_include_directories(xline_async PRIVATE
  ${CMAKE_SOURCE_DIR}/libs)
