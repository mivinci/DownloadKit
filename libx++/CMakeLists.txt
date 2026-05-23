# libx++ — Rust-like infrastructure in C++11
#
# See xpp/CMakeLists.txt for the actual library target. Header-mostly
# value types, RAII helpers, and a ref-counted family inspired by
# Rust's std prelude.

add_subdirectory(xpp)

if(X_BUILD_EXAMPLES)
  add_subdirectory(examples/runtime)
endif()
