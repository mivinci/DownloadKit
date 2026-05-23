add_executable(xpp_runtime_example main.cpp)
target_link_libraries(xpp_runtime_example PRIVATE xpp::main)
set_target_properties(xpp_runtime_example PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)
