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
