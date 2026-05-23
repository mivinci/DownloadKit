/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_coro_test.cpp - Unit tests for xpp::Promise<T> with C++20 coroutines
 */

#include <xpp/compiler.h>

#if XPP_HAS_COROUTINES
#include <coroutine>
#endif

#include <xpp/promise.h>
#include <gtest/gtest.h>

extern "C" {
#include <x/base/event.h>
}

#if XPP_HAS_COROUTINES

/* ── Test Harness ───────────────────────────────────────────────── */

class PromiseCoroTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_loop = xEventLoopCreate();
    m_scope = new xpp::WaitScope(m_loop);
  }
  void TearDown() override {
    delete m_scope;
    xEventLoopDestroy(m_loop);
  }

  xEventLoop m_loop;
  xpp::WaitScope *m_scope;
};

/* ── Basic co_await tests ──────────────────────────────────────────── */

TEST_F(PromiseCoroTest, AwaitReadyInt) {
  auto p = xpp::Promise<int>::resolve(42);
  EXPECT_FALSE(p.await_ready());
}

TEST_F(PromiseCoroTest, AwaitReadyVoid) {
  auto p = xpp::Promise<void>::resolve();
  EXPECT_FALSE(p.await_ready());
}

#else

/* ── Fallback: tests disabled when coroutines not available ──────── */

class PromiseCoroTest : public ::testing::Test {
};

TEST_F(PromiseCoroTest, CoroutinesDisabled) {
  // This test suite requires C++20 coroutine support
  // Skipping all tests when __cpp_coroutines/__cpp_impl_coroutine not defined
  GTEST_SKIP() << "C++20 coroutines not available";
}

#endif // XPP_HAS_COROUTINES
