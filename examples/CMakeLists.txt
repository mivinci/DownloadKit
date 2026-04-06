add_executable(llm_sse_example llm_sse_example.cpp)
target_link_libraries(llm_sse_example PRIVATE xhttp xbase)

add_executable(ws_server_example ws_server_example.cpp)
target_link_libraries(ws_server_example PRIVATE xhttp xbase)

add_executable(ws_client_example ws_client_example.cpp)
target_link_libraries(ws_client_example PRIVATE xhttp xbase)
