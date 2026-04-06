add_executable(llm_sse llm_sse.cpp)
target_link_libraries(llm_sse PRIVATE xhttp xbase)

add_executable(ws_echo_server ws_echo_server.cpp)
target_link_libraries(ws_echo_server PRIVATE xhttp xbase)

add_executable(ws_client ws_client.cpp)
target_link_libraries(ws_client PRIVATE xhttp xbase)
