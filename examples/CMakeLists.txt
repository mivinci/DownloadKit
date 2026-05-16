# ── HTTP examples ──────────────────────────────────────────────────────

add_executable(llm_sse http/llm_sse.cpp)
target_link_libraries(llm_sse PRIVATE xhttp xbase)

add_executable(ws_echo_server http/ws_echo_server.cpp)
target_link_libraries(ws_echo_server PRIVATE xhttp xbase)

add_executable(ws_client http/ws_client.cpp)
target_link_libraries(ws_client PRIVATE xhttp xbase)

add_executable(pc_server http/pc_server.cpp)
target_link_libraries(pc_server PRIVATE xhttp xbase)

# ── P2P examples ──────────────────────────────────────────────────────

add_executable(ice_echo p2p/ice_echo.c)
target_link_libraries(ice_echo PRIVATE xp2p xbase)

add_executable(pc_echo p2p/pc_echo.c)
target_link_libraries(pc_echo PRIVATE xp2p xbase)

add_executable(pc_client p2p/pc_client.cpp)
target_link_libraries(pc_client PRIVATE xp2p xhttp xbase)

add_executable(xfer_echo p2p/xfer_echo.c)
target_link_libraries(xfer_echo PRIVATE xfer xbase)

add_executable(xfer_send p2p/xfer_send.c)
target_link_libraries(xfer_send PRIVATE xfer xbase)

add_executable(xfer_recv p2p/xfer_recv.c)
target_link_libraries(xfer_recv PRIVATE xfer xbase)

add_executable(xfer_signal p2p/xfer_signal.c)
target_link_libraries(xfer_signal PRIVATE xfer xhttp xbase)

add_executable(nat_probe p2p/nat_probe.c)
target_link_libraries(nat_probe PRIVATE xp2p xbase)

# ── Agent examples ────────────────────────────────────────────────────

add_executable(ai_openai agent/ai_openai.cpp)
target_link_libraries(ai_openai PRIVATE xagent xhttp xbase)
# Demo reaches into libx/xagent/provider_private.h for the internal
# dispatcher because session.c has not landed yet. Once session lands,
# this include path should be removed and the demo rewritten against
# xAgentCreate + xAgentSessionCreate.
target_include_directories(ai_openai PRIVATE
  ${CMAKE_SOURCE_DIR}/lib)

# ── JS examples ───────────────────────────────────────────────────────

add_executable(xjs_native_module js/xjs_native_module.c)
target_link_libraries(xjs_native_module PRIVATE xjs)
target_include_directories(xjs_native_module PRIVATE
  ${CMAKE_SOURCE_DIR}/lib)

# ── Line editor examples ──────────────────────────────────────────────

add_executable(xline_async line/xline_async.c)
target_link_libraries(xline_async PRIVATE xline xbase)
target_include_directories(xline_async PRIVATE
  ${CMAKE_SOURCE_DIR}/lib)
