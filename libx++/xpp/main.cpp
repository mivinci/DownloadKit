/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * main.cpp - Default main() entry point for async applications.
 *
 * Link against xpp::main (or the xpp_main CMake target) to get a
 * pre-built main() that creates a Runtime and calls your xpp::main().
 *
 * You provide:
 *   namespace xpp { Promise<int> main(int argc, char *argv[]); }
 *
 * This file provides the boilerplate:
 *
 *   int main(int argc, char *argv[]) {
 *     auto rt = xpp::runtime::Runtime::new_multi_thread();
 *     return rt->block_on([&] { return xpp::main(argc, argv); });
 *   }
 *
 * The Runtime is created with default settings (worker count = CPU
 * cores, lazily spawned). xpp::spawn() is available inside xpp::main()
 * and any coroutine it calls.
 */

#include <xpp/runtime/runtime.h>

namespace xpp {

/**
 * @brief User-defined async entry point.
 *
 * Implement this in your application. It runs inside the Runtime's
 * block_on context, so xpp::spawn() is available.
 *
 * @return Exit code (passed to std::exit).
 */
extern Promise<int> main(int argc, char *argv[]);

} // namespace xpp

int main(int argc, char *argv[]) {
  auto rt = xpp::runtime::Runtime::new_multi_thread();
  return rt->block_on([&]() -> xpp::Promise<int> {
    return xpp::main(argc, argv);
  });
}
