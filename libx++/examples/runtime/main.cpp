/*
 * runtime example — demonstrates xpp::spawn, co_await, and xpp::main.
 *
 * Build:
 *   cmake -B build -DCMAKE_CXX_STANDARD=20 -DX_BUILD_EXAMPLES=ON
 *   cmake --build build --target xpp_runtime_example
 *
 * Run:
 *   ./build/libx++/examples/runtime/xpp_runtime_example
 */

#include <xpp/runtime.h>
#include <cstdio>

using xpp::Promise;
using xpp::JoinHandle;
using xpp::spawn;

// A simple async task that "computes" a value.
Promise<int> compute(int x) {
  co_await xpp::yield();  // simulate async work
  co_return x * x;
}

// Fan-out: spawn multiple tasks, await all results.
Promise<int> fan_out() {
  auto h1 = spawn(compute(3));
  auto h2 = spawn(compute(4));
  auto h3 = spawn(compute(5));

  int a = co_await h1;
  int b = co_await h2;
  int c = co_await h3;

  co_return a + b + c;  // 9 + 16 + 25 = 50
}

// Entry point — linked via xpp::main, no boilerplate needed.
namespace xpp {
Promise<int> main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printf("spawning tasks...\n");

  int result = co_await fan_out();
  printf("fan_out result: %d (expected 50)\n", result);

  // Sequential spawns.
  int sum = 0;
  for (int i = 1; i <= 10; ++i) {
    auto h = spawn(compute(i));
    sum += co_await h;
  }
  printf("sum of squares 1..10: %d (expected 385)\n", sum);

  co_return 0;
}
} // namespace xpp
