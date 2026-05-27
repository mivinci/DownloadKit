/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime_test.cpp - Unit tests for xpp::Runtime
 */

#include <xpp/compiler.h>

#if XPP_HAS_COROUTINES

#include <xpp/runtime.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

/* ── Test fixture ─────────────────────────────────────────────────── */

class RuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = new xpp::Runtime(2);  // 2 workers
  }
  void TearDown() override {
    delete m_rt;
  }
  xpp::Runtime *m_rt;
};

/* ── block_on ─────────────────────────────────────────────────────── */

TEST_F(RuntimeTest, BlockOnResolved) {
  int val = m_rt->block_on(xpp::Promise<int>::resolve(42));
  EXPECT_EQ(val, 42);
}

TEST_F(RuntimeTest, BlockOnVoid) {
  m_rt->block_on(xpp::Promise<void>::resolve());
}

TEST_F(RuntimeTest, BlockOnCoroutine) {
  auto coro = []() -> xpp::Promise<int> {
    co_return 99;
  };
  EXPECT_EQ(m_rt->block_on(coro()), 99);
}

/* ── spawn + co_await ─────────────────────────────────────────────── */

TEST_F(RuntimeTest, SpawnAndAwait) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto handle = m_rt->spawn([]() -> xpp::Promise<int> {
      co_return 7;
    });
    int result = co_await handle;
    co_return result;
  };

  EXPECT_EQ(m_rt->block_on(orchestrate()), 7);
}

TEST_F(RuntimeTest, SpawnVoid) {
  std::atomic<bool> done{false};

  auto orchestrate = [&]() -> xpp::Promise<void> {
    auto handle = m_rt->spawn([&]() -> xpp::Promise<void> {
      done.store(true);
      co_return;
    });
    co_await handle;
  };

  m_rt->block_on(orchestrate());
  EXPECT_TRUE(done.load());
}

/* ── spawn multiple ───────────────────────────────────────────────── */

TEST_F(RuntimeTest, SpawnMultiple) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto h1 = m_rt->spawn([]() -> xpp::Promise<int> { co_return 9; });
    auto h2 = m_rt->spawn([]() -> xpp::Promise<int> { co_return 16; });
    int a = co_await h1;
    int b = co_await h2;
    co_return a + b;
  };

  EXPECT_EQ(m_rt->block_on(orchestrate()), 25);
}

/* ── detach ───────────────────────────────────────────────────────── */

TEST_F(RuntimeTest, Detach) {
  std::atomic<int> counter{0};

  auto work = [&]() -> xpp::Promise<void> {
    counter.fetch_add(1);
    co_return;
  };

  auto orchestrate = [&]() -> xpp::Promise<void> {
    auto handle = m_rt->spawn([&]() -> xpp::Promise<void> {
      counter.fetch_add(1);
      co_return;
    });
    handle.detach();
    co_return;
  };

  m_rt->block_on(orchestrate());

  // Give the detached task time to complete.
  // (In real code you'd use a barrier or similar.)
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(counter.load(), 1);
}

/* ── spawn runs on different thread ───────────────────────────────── */

TEST_F(RuntimeTest, SpawnRunsOnWorker) {
  auto main_tid = std::this_thread::get_id();

  auto orchestrate = [&]() -> xpp::Promise<bool> {
    auto handle = m_rt->spawn([&]() -> xpp::Promise<bool> {
      co_return std::this_thread::get_id() != main_tid;
    });
    co_return co_await handle;
  };

  EXPECT_TRUE(m_rt->block_on(orchestrate()));
}

#else

#include <gtest/gtest.h>

TEST(RuntimeCoroTest, CoroutinesDisabled) {
  GTEST_SKIP() << "C++20 coroutines not available";
}

#endif // XPP_HAS_COROUTINES
