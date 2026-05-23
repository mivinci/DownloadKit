/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex_test.cpp - Tests for async xpp::Mutex<T>.
 */

#include <xpp/compiler.h>

#if XPP_HAS_COROUTINES

#include <xpp/mutex.h>
#include <xpp/runtime.h>
#include <gtest/gtest.h>

#include <atomic>

class AsyncMutexTest : public ::testing::Test {
protected:
  void SetUp() override { m_rt = new xpp::Runtime(4); }
  void TearDown() override { delete m_rt; }
  xpp::Runtime *m_rt;
};

/* ── Basic lock/unlock ────────────────────────────────────────────── */

TEST_F(AsyncMutexTest, LockUnlock) {
  xpp::Mutex<int> m(0);

  auto coro = [&]() -> xpp::Promise<int> {
    auto guard = co_await m.lock();
    *guard = 42;
    co_return *guard;
  };

  EXPECT_EQ(m_rt->block_on(coro()), 42);
}

/* ── try_lock ─────────────────────────────────────────────────────── */

TEST_F(AsyncMutexTest, TryLockSuccess) {
  xpp::Mutex<int> m(10);
  auto result = m.try_lock();
  EXPECT_TRUE(result.is_some());
  EXPECT_EQ(*result.unwrap(), 10);
}

TEST_F(AsyncMutexTest, TryLockContended) {
  xpp::Mutex<int> m(0);
  auto guard = m.try_lock();
  EXPECT_TRUE(guard.is_some());

  // While held, try_lock should fail.
  auto second = m.try_lock();
  EXPECT_TRUE(second.is_none());
}

/* ── Mutual exclusion under concurrency ───────────────────────────── */

TEST_F(AsyncMutexTest, MutualExclusion) {
  xpp::Mutex<int> m(0);
  const int N = 100;

  auto increment = [&]() -> xpp::Promise<void> {
    auto guard = co_await m.lock();
    int val = *guard;
    co_await xpp::yield();  // simulate some work / yield point
    *guard = val + 1;
  };

  auto orchestrate = [&]() -> xpp::Promise<int> {
    xpp::JoinHandle<void> *handles = new xpp::JoinHandle<void>[N];
    for (int i = 0; i < N; ++i) {
      handles[i] = m_rt->spawn(increment());
    }
    for (int i = 0; i < N; ++i) {
      co_await handles[i];
    }
    delete[] handles;

    auto guard = co_await m.lock();
    co_return *guard;
  };

  EXPECT_EQ(m_rt->block_on(orchestrate()), N);
}

/* ── Guard drop releases lock ─────────────────────────────────────── */

TEST_F(AsyncMutexTest, GuardDropReleasesLock) {
  xpp::Mutex<int> m(0);

  auto coro = [&]() -> xpp::Promise<void> {
    {
      auto guard = co_await m.lock();
      *guard = 1;
    } // guard dropped — lock released

    {
      auto guard = co_await m.lock();
      *guard = 2;
    }
  };

  m_rt->block_on(coro());

  auto guard = m.try_lock();
  EXPECT_TRUE(guard.is_some());
  EXPECT_EQ(*guard.unwrap(), 2);
}

/* ── FIFO ordering ────────────────────────────────────────────────── */

TEST_F(AsyncMutexTest, FIFOOrdering) {
  xpp::Mutex<int> m(0);
  std::atomic<int> order{0};

  auto writer = [&](int id) -> xpp::Promise<void> {
    auto guard = co_await m.lock();
    *guard = id;
    // Record order of acquisition.
    order.fetch_add(1, std::memory_order_relaxed);
  };

  auto coro = [&]() -> xpp::Promise<void> {
    // Hold the lock while spawning waiters.
    auto guard = co_await m.lock();

    auto h1 = m_rt->spawn(writer(1));
    auto h2 = m_rt->spawn(writer(2));
    auto h3 = m_rt->spawn(writer(3));

    // Release lock — waiters should be served.
    guard = xpp::MutexGuard<int>{};

    co_await h1;
    co_await h2;
    co_await h3;
  };

  // This test just verifies no deadlock/crash.
  m_rt->block_on(coro());
}

#else

#include <gtest/gtest.h>

TEST(AsyncMutexTest, CoroutinesDisabled) {
  GTEST_SKIP() << "C++20 coroutines not available";
}

#endif // XPP_HAS_COROUTINES
