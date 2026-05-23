/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * main.cpp - Default main() entry point for async applications.
 *
 * Link against xpp::main (or the x++_main CMake target) to get a
 * pre-built main() that creates a Runtime and calls your xpp_main().
 *
 * You provide:
 *   xpp::Promise<int> xpp_main();
 *
 * This file provides the boilerplate:
 *   int main() {
 *     xpp::Runtime rt;
 *     return rt.block_on(xpp_main());
 *   }
 *
 * The Runtime is created with default settings (worker count = CPU
 * cores, lazily spawned). xpp::spawn() is available inside xpp_main
 * and any coroutine it calls.
 */

#include <xpp/runtime.h>

/**
 * @brief User-defined async entry point.
 *
 * Implement this in your application. It runs inside the Runtime's
 * block_on context, so xpp::spawn() is available.
 *
 * @return Exit code (passed to std::exit).
 */
extern xpp::Promise<int> xpp_main();

int main() {
  xpp::Runtime rt;
  return rt.block_on(xpp_main());
}
