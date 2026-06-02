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

#include <gtest/gtest.h>
#include <xpp/runtime/runtime.h>

extern "C" {
#include <x/base/event.h>
}

#if XPP_HAS_COROUTINES

/* ── Test Harness ───────────────────────────────────────────────── */

class PromiseCoroTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = new xpp::runtime::Runtime(1);
  }
  void TearDown() override {
    delete m_rt;
  }

  // Drive a coroutine promise to completion (blocking drive now lives in
  // Runtime::block_on; Promise::wait() was removed).
  template <class T> T wait(xpp::Promise<T> p) {
    return m_rt->block_on(std::move(p));
  }

  xpp::runtime::Runtime *m_rt;
};

/* ── Awaitable protocol tests ─────────────────────────────────────── */

TEST_F(PromiseCoroTest, AwaitReadyAlwaysFalse) {
  // operator co_await returns an Awaiter; verify it exists and works
  // by doing a simple co_await on a resolved promise.
  auto coro = [&]() -> xpp::Promise<int> { co_return co_await xpp::Promise<int>::resolve(42); }();
  EXPECT_EQ(wait(std::move(coro)), 42);
}

/* ── co_await on resolved promises ────────────────────────────────── */

TEST_F(PromiseCoroTest, CoAwaitResolvedInt) {
  auto coro = [&]() -> xpp::Promise<int> {
    int val = co_await xpp::Promise<int>::resolve(42);
    co_return val;
  }();

  EXPECT_EQ(wait(std::move(coro)), 42);
}

TEST_F(PromiseCoroTest, CoAwaitResolvedVoid) {
  bool reached = false;
  auto coro    = [&]() -> xpp::Promise<void> {
    co_await xpp::Promise<void>::resolve();
    reached = true;
  }();

  wait(std::move(coro));
  EXPECT_TRUE(reached);
}

/* ── co_await on deferred promises (eval) ─────────────────────────── */

TEST_F(PromiseCoroTest, CoAwaitEval) {
  auto coro = [&]() -> xpp::Promise<int> {
    int val = co_await xpp::Promise<void>::eval([] { return 99; });
    co_return val;
  }();

  EXPECT_EQ(wait(std::move(coro)), 99);
}

/* ── co_await with chained then ───────────────────────────────────── */

TEST_F(PromiseCoroTest, CoAwaitAfterThen) {
  auto coro = [&]() -> xpp::Promise<int> {
    int val = co_await xpp::Promise<int>::resolve(10).then([](int x) { return x * 3; });
    co_return val;
  }();

  EXPECT_EQ(wait(std::move(coro)), 30);
}

/* ── Multiple co_await in one coroutine ───────────────────────────── */

TEST_F(PromiseCoroTest, MultipleCoAwait) {
  auto coro = [&]() -> xpp::Promise<int> {
    int a = co_await xpp::Promise<int>::resolve(10);
    int b = co_await xpp::Promise<int>::resolve(20);
    co_return a + b;
  }();

  EXPECT_EQ(wait(std::move(coro)), 30);
}

/* ── co_await on Resolver (async resolve) ─────────────────────────── */

TEST_F(PromiseCoroTest, CoAwaitResolver) {
  auto pr   = xpp::Promise<int>::make();
  auto coro = [&]() -> xpp::Promise<int> {
    int val = co_await std::move(pr.promise);
    co_return val;
  }();

  pr.resolver.resolve(77);
  EXPECT_EQ(wait(std::move(coro)), 77);
}

/* ── co_await flatten (then returns Promise) ──────────────────────── */

TEST_F(PromiseCoroTest, CoAwaitFlatten) {
  auto coro = [&]() -> xpp::Promise<int> {
    int val = co_await xpp::Promise<int>::resolve(5).then(
      [](int x) { return xpp::Promise<int>::resolve(x * 4); });
    co_return val;
  }();

  EXPECT_EQ(wait(std::move(coro)), 20);
}

/* ── Nested coroutines ────────────────────────────────────────────── */

TEST_F(PromiseCoroTest, NestedCoroutine) {
  auto inner = []() -> xpp::Promise<int> { co_return 7; };

  auto outer = [&]() -> xpp::Promise<int> {
    int          a = co_await inner();
    int          b = co_await inner();
    co_return a *b;
  }();

  EXPECT_EQ(wait(std::move(outer)), 49);
}

/* ── Coroutine returning void ─────────────────────────────────────── */

TEST_F(PromiseCoroTest, CoroutineReturnsVoid) {
  int  side_effect = 0;
  auto coro        = [&]() -> xpp::Promise<void> {
    int val     = co_await xpp::Promise<int>::resolve(42);
    side_effect = val;
  }();

  wait(std::move(coro));
  EXPECT_EQ(side_effect, 42);
}

#else

/* ── Fallback: tests disabled when coroutines not available ──────── */

class PromiseCoroTest : public ::testing::Test {};

TEST_F(PromiseCoroTest, CoroutinesDisabled) {
  GTEST_SKIP() << "C++20 coroutines not available";
}

#endif // XPP_HAS_COROUTINES
