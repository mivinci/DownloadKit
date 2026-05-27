/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_test.cpp - Unit tests for xpp::Promise<T>
 */

#include <xpp/runtime.h>

#include <gtest/gtest.h>

extern "C" {
#include <x/base/event.h>
}

class PromiseTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_loop = xEventLoopCreate();
    m_guard = new xpp::EnterGuard(nullptr, nullptr, m_loop);
  }
  void TearDown() override {
    delete m_guard;
    xEventLoopDestroy(m_loop);
  }

  xEventLoop    m_loop;
  xpp::EnterGuard *m_guard;
};

/* ── resolve + wait ────────────────────────────────────────────────── */

TEST_F(PromiseTest, ResolveInt) {
  auto p = xpp::Promise<int>::resolve(42);
  EXPECT_EQ(p.wait(*m_guard), 42);
}

TEST_F(PromiseTest, ResolveVoid) {
  auto p = xpp::Promise<void>::resolve();
  p.wait(*m_guard);  // should not hang
}

/* ── eval ────────────────────────────────────────────────────── */

TEST_F(PromiseTest, EvalLaterInt) {
  auto p = xpp::Promise<void>::eval([] { return 42; });
  EXPECT_EQ(p.wait(*m_guard), 42);
}

TEST_F(PromiseTest, EvalLaterVoid) {
  bool executed = false;
  auto p = xpp::Promise<void>::eval([&] { executed = true; });
  EXPECT_FALSE(executed);
  p.wait(*m_guard);
  EXPECT_TRUE(executed);
}

/* ── then (single) ─────────────────────────────────────────────────── */

TEST_F(PromiseTest, ThenIntToInt) {
  int result = xpp::Promise<int>::resolve(10)
    .then([](int x) { return x + 1; })
    .wait(*m_guard);
  EXPECT_EQ(result, 11);
}

TEST_F(PromiseTest, ThenIntToVoid) {
  int captured = 0;
  xpp::Promise<int>::resolve(42)
    .then([&](int x) { captured = x; })
    .wait(*m_guard);
  EXPECT_EQ(captured, 42);
}

TEST_F(PromiseTest, ThenVoidToInt) {
  int result = xpp::Promise<void>::resolve()
    .then([] { return 99; })
    .wait(*m_guard);
  EXPECT_EQ(result, 99);
}

TEST_F(PromiseTest, ThenVoidToVoid) {
  bool done = false;
  xpp::Promise<void>::resolve()
    .then([&] { done = true; })
    .wait(*m_guard);
  EXPECT_TRUE(done);
}

/* ── then (chain) ──────────────────────────────────────────────────── */

TEST_F(PromiseTest, ThenChain) {
  int result = xpp::Promise<int>::resolve(1)
    .then([](int x) { return x + 1; })
    .then([](int x) { return x * 3; })
    .then([](int x) { return x - 1; })
    .wait(*m_guard);
  EXPECT_EQ(result, 5);  // (1+1)*3 - 1 = 5
}

/* ── eval + then ─────────────────────────────────────────────── */

TEST_F(PromiseTest, EvalLaterThen) {
  int result = xpp::Promise<void>::eval([] { return 10; })
    .then([](int x) { return x * 2; })
    .wait(*m_guard);
  EXPECT_EQ(result, 20);
}

/* ── create_promise + resolver ─────────────────────────────────────── */

TEST_F(PromiseTest, ResolverBasic) {
  auto pair = xpp::Promise<int>::make();
  pair.resolver.resolve(99);
  EXPECT_EQ(pair.promise.wait(*m_guard), 99);
}

TEST_F(PromiseTest, ResolverVoid) {
  auto pair = xpp::Promise<void>::make();
  pair.resolver.resolve();
  pair.promise.wait(*m_guard);  // should not hang
}

/* ── yield ─────────────────────────────────────────────────────────── */

TEST_F(PromiseTest, Yield) {
  xpp::yield().wait(*m_guard);  // should not hang
}

/* ── flatten (then returns Promise<T>) ────────────────────────────────── */

TEST_F(PromiseTest, FlattenPromise) {
  int result = xpp::Promise<int>::resolve(10)
    .then([](int x) {
      return xpp::Promise<int>::resolve(x + 1);  // returns Promise<int>
    })
    .wait(*m_guard);
  EXPECT_EQ(result, 11);
}

TEST_F(PromiseTest, FlattenPromiseVoid) {
  bool done = false;
  xpp::Promise<int>::resolve(42)
    .then([&](int x) {
      (void)x;
      done = true;
      return xpp::Promise<void>::resolve();  // returns Promise<void>
    })
    .wait(*m_guard);
  EXPECT_TRUE(done);
}

TEST_F(PromiseTest, FlattenChained) {
  int result = xpp::Promise<int>::resolve(1)
    .then([](int x) {
      return xpp::Promise<int>::resolve(x * 10);
    })
    .then([](int x) {
      return xpp::Promise<int>::resolve(x + 5);
    })
    .wait(*m_guard);
  EXPECT_EQ(result, 15);  // 1*10 + 5
}

/* ── Promise<void> operations ──────────────────────────────────────── */

TEST_F(PromiseTest, VoidDiscard) {
  bool executed = false;
  xpp::Promise<void>::eval([&] {
    executed = true;
    return 42;
  }).discard().wait(*m_guard);
  EXPECT_TRUE(executed);
}

/* ── Move semantics ───────────────────────────────────────────────────── */

TEST_F(PromiseTest, MovePromise) {
  auto p1 = xpp::Promise<int>::resolve(42);
  auto p2 = std::move(p1);
  EXPECT_FALSE(static_cast<bool>(p1));
  EXPECT_TRUE(static_cast<bool>(p2));
  EXPECT_EQ(p2.wait(*m_guard), 42);
}

TEST_F(PromiseTest, MoveAssign) {
  auto p1 = xpp::Promise<int>::resolve(1);
  auto p2 = xpp::Promise<int>::resolve(2);
  p1 = std::move(p2);
  EXPECT_FALSE(static_cast<bool>(p2));
  EXPECT_EQ(p1.wait(*m_guard), 2);  // p1 now holds p2's node
}

/* ── Resolver with then ───────────────────────────────────────────────── */

TEST_F(PromiseTest, ResolverThen) {
  auto pair = xpp::Promise<int>::make();
  auto chained = pair.promise
    .then([](int x) { return x * 2; });
  pair.resolver.resolve(21);
  EXPECT_EQ(chained.wait(*m_guard), 42);
}
