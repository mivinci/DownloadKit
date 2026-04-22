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
target_link_libraries(ai_openai PRIVATE xai xhttp xbase)
# Demo reaches into modules/xai/provider_private.h for the internal
# dispatcher because session.c has not landed yet. Once session lands,
# this include path should be removed and the demo rewritten against
# xAiAgentCreate + xAiSessionCreate.
target_include_directories(ai_openai PRIVATE
  ${CMAKE_SOURCE_DIR}/modules)
