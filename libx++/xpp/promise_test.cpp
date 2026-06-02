/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_test.cpp - Unit tests for xpp::Promise<T>
 */

#include <xpp/runtime/runtime.h>

#include <gtest/gtest.h>

extern "C" {
#include <x/base/event.h>
}

class PromiseTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = xpp::runtime::Runtime::new_multi_thread(1).into_raw();
  }
  void TearDown() override {
    delete m_rt;
  }

  // Drive a promise to completion on the runtime (replaces the old
  // Promise::wait(); blocking drive now lives in Runtime::block_on).
  template <class T> T wait(xpp::Promise<T> p) {
    return m_rt->block_on(std::move(p));
  }

  xpp::runtime::Runtime *m_rt;
};

/* ── resolve + wait ────────────────────────────────────────────────── */

TEST_F(PromiseTest, ResolveInt) {
  EXPECT_EQ(wait(xpp::Promise<int>::resolve(42)), 42);
}

TEST_F(PromiseTest, ResolveVoid) {
  wait(xpp::Promise<void>::resolve()); // should not hang
}

/* ── eval ────────────────────────────────────────────────────── */

TEST_F(PromiseTest, EvalLaterInt) {
  EXPECT_EQ(wait(xpp::Promise<void>::eval([] { return 42; })), 42);
}

TEST_F(PromiseTest, EvalLaterVoid) {
  bool executed = false;
  auto p        = xpp::Promise<void>::eval([&] { executed = true; });
  EXPECT_FALSE(executed);
  wait(std::move(p));
  EXPECT_TRUE(executed);
}

/* ── then (single) ─────────────────────────────────────────────────── */

TEST_F(PromiseTest, ThenIntToInt) {
  int result = wait(xpp::Promise<int>::resolve(10).then([](int x) { return x + 1; }));
  EXPECT_EQ(result, 11);
}

TEST_F(PromiseTest, ThenIntToVoid) {
  int captured = 0;
  wait(xpp::Promise<int>::resolve(42).then([&](int x) { captured = x; }));
  EXPECT_EQ(captured, 42);
}

TEST_F(PromiseTest, ThenVoidToInt) {
  int result = wait(xpp::Promise<void>::resolve().then([] { return 99; }));
  EXPECT_EQ(result, 99);
}

TEST_F(PromiseTest, ThenVoidToVoid) {
  bool done = false;
  wait(xpp::Promise<void>::resolve().then([&] { done = true; }));
  EXPECT_TRUE(done);
}

/* ── then (chain) ──────────────────────────────────────────────────── */

TEST_F(PromiseTest, ThenChain) {
  int result = wait(xpp::Promise<int>::resolve(1)
                      .then([](int x) { return x + 1; })
                      .then([](int x) { return x * 3; })
                      .then([](int x) { return x - 1; }));
  EXPECT_EQ(result, 5); // (1+1)*3 - 1 = 5
}

/* ── eval + then ─────────────────────────────────────────────── */

TEST_F(PromiseTest, EvalLaterThen) {
  int result =
    wait(xpp::Promise<void>::eval([] { return 10; }).then([](int x) { return x * 2; }));
  EXPECT_EQ(result, 20);
}

/* ── create_promise + resolver ─────────────────────────────────────── */

TEST_F(PromiseTest, ResolverBasic) {
  auto pair = xpp::Promise<int>::make();
  pair.resolver.resolve(99);
  EXPECT_EQ(wait(std::move(pair.promise)), 99);
}

TEST_F(PromiseTest, ResolverVoid) {
  auto pair = xpp::Promise<void>::make();
  pair.resolver.resolve();
  wait(std::move(pair.promise)); // should not hang
}

/* ── yield ─────────────────────────────────────────────────────────── */

TEST_F(PromiseTest, Yield) {
  wait(xpp::yield()); // should not hang
}

/* ── flatten (then returns Promise<T>) ────────────────────────────────── */

TEST_F(PromiseTest, FlattenPromise) {
  int result = wait(xpp::Promise<int>::resolve(10).then([](int x) {
    return xpp::Promise<int>::resolve(x + 1); // returns Promise<int>
  }));
  EXPECT_EQ(result, 11);
}

TEST_F(PromiseTest, FlattenPromiseVoid) {
  bool done = false;
  wait(xpp::Promise<int>::resolve(42).then([&](int x) {
    (void)x;
    done = true;
    return xpp::Promise<void>::resolve(); // returns Promise<void>
  }));
  EXPECT_TRUE(done);
}

TEST_F(PromiseTest, FlattenChained) {
  int result = wait(xpp::Promise<int>::resolve(1)
                      .then([](int x) { return xpp::Promise<int>::resolve(x * 10); })
                      .then([](int x) { return xpp::Promise<int>::resolve(x + 5); }));
  EXPECT_EQ(result, 15); // 1*10 + 5
}
