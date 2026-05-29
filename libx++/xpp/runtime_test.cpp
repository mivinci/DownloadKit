/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime_test.cpp - C++11-compatible unit tests for xpp::Runtime.
 *
 * These tests use only then()/resolve()/eval() — no coroutines.
 * Coroutine-based tests (spawn, co_await) live in runtime_coro_test.cpp.
 */

#include <gtest/gtest.h>
#include <xpp/runtime.h>

#include <atomic>
#include <thread>

/* ── Test fixture ─────────────────────────────────────────────────── */

class RuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = new xpp::Runtime(2);
  }
  void TearDown() override {
    delete m_rt;
  }
  xpp::Runtime *m_rt;
};

/* ── block_on with resolved values ────────────────────────────────── */

TEST_F(RuntimeTest, BlockOnResolvedInt) {
  int val = m_rt->block_on(xpp::Promise<int>::resolve(42));
  EXPECT_EQ(val, 42);
}

TEST_F(RuntimeTest, BlockOnResolvedVoid) {
  m_rt->block_on(xpp::Promise<void>::resolve());
}

TEST_F(RuntimeTest, BlockOnResolvedString) {
  std::string s = m_rt->block_on(xpp::Promise<std::string>::resolve("hello"));
  EXPECT_EQ(s, "hello");
}

/* ── block_on with then() chains ──────────────────────────────────── */

TEST_F(RuntimeTest, BlockOnThen) {
  auto p = xpp::Promise<int>::resolve(10).then([](int x) { return x * 2; });
  EXPECT_EQ(m_rt->block_on(std::move(p)), 20);
}

TEST_F(RuntimeTest, BlockOnThenChain) {
  auto p = xpp::Promise<int>::resolve(3).then([](int x) { return x + 1; }).then([](int x) {
    return x * x;
  });
  EXPECT_EQ(m_rt->block_on(std::move(p)), 16); // (3+1)^2
}

TEST_F(RuntimeTest, BlockOnThenVoidToInt) {
  auto p = xpp::Promise<void>::resolve().then([] { return 99; });
  EXPECT_EQ(m_rt->block_on(std::move(p)), 99);
}

TEST_F(RuntimeTest, BlockOnThenIntToVoid) {
  std::atomic<int> side_effect{0};
  auto             p = xpp::Promise<int>::resolve(5).then([&](int x) { side_effect.store(x); });
  m_rt->block_on(std::move(p));
  EXPECT_EQ(side_effect.load(), 5);
}

/* ── block_on with eval() ─────────────────────────────────────────── */

TEST_F(RuntimeTest, BlockOnEval) {
  auto p = xpp::Promise<void>::eval([] { return 7; });
  EXPECT_EQ(m_rt->block_on(std::move(p)), 7);
}

TEST_F(RuntimeTest, BlockOnEvalThenChain) {
  auto p =
    xpp::Promise<void>::eval([] { return 2; }).then([](int x) { return x * 3; }).then([](int x) {
      return x + 1;
    });
  EXPECT_EQ(m_rt->block_on(std::move(p)), 7); // 2*3+1
}

/* ── block_on with nested promises (then returning Promise) ───────── */

TEST_F(RuntimeTest, BlockOnThenReturnsPromise) {
  auto p =
    xpp::Promise<int>::resolve(5).then([](int x) { return xpp::Promise<int>::resolve(x * 10); });
  EXPECT_EQ(m_rt->block_on(std::move(p)), 50);
}

/* ── Runtime basics ───────────────────────────────────────────────── */

TEST_F(RuntimeTest, CurrentOutsideRuntime) {
  // Outside block_on, no runtime is active on this thread.
  EXPECT_EQ(xpp::Runtime::current(), nullptr);
}

/* ── spawn_blocking ───────────────────────────────────────────────── */

TEST_F(RuntimeTest, SpawnBlockingReturnsInt) {
  int val = m_rt->block_on([&]() { return xpp::spawn_blocking([] { return 42; }); });
  EXPECT_EQ(val, 42);
}

TEST_F(RuntimeTest, SpawnBlockingReturnsString) {
  std::string s =
    m_rt->block_on([&]() { return xpp::spawn_blocking([] { return std::string("blocking"); }); });
  EXPECT_EQ(s, "blocking");
}

TEST_F(RuntimeTest, SpawnBlockingVoid) {
  std::atomic<bool> ran{false};
  m_rt->block_on([&]() { return xpp::spawn_blocking([&] { ran.store(true); }); });
  EXPECT_TRUE(ran.load());
}

TEST_F(RuntimeTest, SpawnBlockingThenChain) {
  int val = m_rt->block_on(
    [&]() { return xpp::spawn_blocking([] { return 10; }).then([](int x) { return x * 3; }); });
  EXPECT_EQ(val, 30);
}

TEST_F(RuntimeTest, SpawnBlockingRunsOffEventLoopThread) {
  // Confirm the blocking lambda runs on a different thread than block_on.
  std::thread::id blocking_tid{};
  std::thread::id loop_tid = std::this_thread::get_id();

  m_rt->block_on(
    [&]() { return xpp::spawn_blocking([&] { blocking_tid = std::this_thread::get_id(); }); });
  EXPECT_NE(blocking_tid, loop_tid);
  EXPECT_NE(blocking_tid, std::thread::id{});
}

TEST_F(RuntimeTest, SpawnBlockingConcurrent) {
  // Each spawn_blocking is sequential in block_on, but both execute
  // on the xTaskGroup thread pool.  Verify two calls succeed.
  int val = m_rt->block_on([&]() {
    return xpp::spawn_blocking([] { return 3; }).then([&](int a) {
      return xpp::spawn_blocking([a] { return a + 5; });
    });
  });
  EXPECT_EQ(val, 8); // 3 + 5
}

TEST_F(RuntimeTest, SpawnBlockingAcceptsNonTrivialCallable) {
  // Ensure spawn_blocking correctly decays and moves the callable.
  struct Stateful {
    int extra;
    int operator()() const {
      return extra * 5;
    }
  };
  Stateful s{11};
  int      val = m_rt->block_on([&]() { return xpp::spawn_blocking(s); });
  EXPECT_EQ(val, 55);
}
