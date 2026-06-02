/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime_test.cpp - Unit tests for xpp::runtime::Runtime
 */

#include <xpp/compiler.h>

#if XPP_HAS_COROUTINES

#include <gtest/gtest.h>
#include <xpp/runtime/runtime.h>
#include <xpp/task.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

/* ── Test fixture ─────────────────────────────────────────────────── */

class RuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = xpp::runtime::Runtime::new_multi_thread(2).into_raw(); // 2 workers
  }
  void TearDown() override {
    delete m_rt;
  }
  xpp::runtime::Runtime *m_rt;
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
  auto coro = []() -> xpp::Promise<int> { co_return 99; };
  EXPECT_EQ(m_rt->block_on(coro()), 99);
}

/* ── spawn + co_await ─────────────────────────────────────────────── */

TEST_F(RuntimeTest, SpawnAndAwait) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto p      = m_rt->spawn([]() -> xpp::Promise<int> { co_return 7; });
    int  result = co_await p;
    co_return result;
  };

  EXPECT_EQ(m_rt->block_on(orchestrate()), 7);
}

TEST_F(RuntimeTest, SpawnVoid) {
  std::atomic<bool> done{false};

  auto orchestrate = [&]() -> xpp::Promise<void> {
    auto p = m_rt->spawn([&]() -> xpp::Promise<void> {
      done.store(true);
      co_return;
    });
    co_await p;
  };

  m_rt->block_on(orchestrate());
  EXPECT_TRUE(done.load());
}

/* ── spawn multiple ───────────────────────────────────────────────── */

TEST_F(RuntimeTest, SpawnMultiple) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto h1 = m_rt->spawn([]() -> xpp::Promise<int> { co_return 9; });
    auto h2 = m_rt->spawn([]() -> xpp::Promise<int> { co_return 16; });
    int  a  = co_await h1;
    int  b  = co_await h2;
    co_return a + b;
  };

  EXPECT_EQ(m_rt->block_on(orchestrate()), 25);
}

/* ── detach (drop = detach) ───────────────────────────────────────── */

TEST_F(RuntimeTest, Detach) {
  std::atomic<int> counter{0};

  auto orchestrate = [&]() -> xpp::Promise<void> {
    // Dropping the Promise detaches the task (runs to completion).
    m_rt->spawn([&]() -> xpp::Promise<void> {
      counter.fetch_add(1);
      co_return;
    });
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
    auto p = m_rt->spawn(
      [&]() -> xpp::Promise<bool> { co_return std::this_thread::get_id() != main_tid; });
    co_return co_await p;
  };

  EXPECT_TRUE(m_rt->block_on(orchestrate()));
}

/* ── spawn_blocking + co_await ────────────────────────────────────── */

TEST_F(RuntimeTest, SpawnBlockingAwaitInt) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    int v = co_await xpp::spawn_blocking([] { return 21; });
    co_return v * 2;
  };
  EXPECT_EQ(m_rt->block_on(orchestrate), 42);
}

TEST_F(RuntimeTest, SpawnBlockingAwaitVoid) {
  std::atomic<bool> ran{false};
  auto              orchestrate = [&]() -> xpp::Promise<void> {
    co_await xpp::spawn_blocking([&] { ran.store(true); });
    co_return;
  };
  m_rt->block_on(orchestrate);
  EXPECT_TRUE(ran.load());
}

TEST_F(RuntimeTest, SpawnBlockingAwaitString) {
  auto orchestrate = [&]() -> xpp::Promise<std::string> {
    auto s = co_await xpp::spawn_blocking([] { return std::string("hello"); });
    co_return s + ",coro";
  };
  EXPECT_EQ(m_rt->block_on(orchestrate), "hello,coro");
}

TEST_F(RuntimeTest, SpawnBlockingRunsOffWorkerThread) {
  // Coroutine resumes on a worker; the blocking lambda must run on a
  // different (m_group blocking-pool) thread.
  std::thread::id orchestrate_tid{};
  std::thread::id blocking_tid{};

  auto orchestrate = [&]() -> xpp::Promise<void> {
    orchestrate_tid = std::this_thread::get_id();
    co_await xpp::spawn_blocking([&] { blocking_tid = std::this_thread::get_id(); });
    co_return;
  };

  m_rt->block_on(orchestrate);
  EXPECT_NE(blocking_tid, std::thread::id{});
  EXPECT_NE(blocking_tid, orchestrate_tid);
}

TEST_F(RuntimeTest, SpawnBlockingMixedWithSpawn) {
  // Combine spawn() (async coroutine) and spawn_blocking() (off-loop work)
  // and join them via co_await.
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto async_part    = m_rt->spawn([]() -> xpp::Promise<int> { co_return 4; });
    auto blocking_part = xpp::spawn_blocking([] { return 38; });
    int  a             = co_await async_part;
    int  b             = co_await blocking_part;
    co_return a + b;
  };
  EXPECT_EQ(m_rt->block_on(orchestrate), 42);
}

TEST_F(RuntimeTest, SpawnBlockingChainedAwaits) {
  // Two spawn_blocking calls in sequence, awaited in a single coroutine.
  // Verifies the blocking pool can serve consecutive requests cleanly.
  auto orchestrate = [&]() -> xpp::Promise<int> {
    int a = co_await xpp::spawn_blocking([] { return 10; });
    int b = co_await xpp::spawn_blocking([a] { return a + 5; });
    co_return b;
  };
  EXPECT_EQ(m_rt->block_on(orchestrate), 15);
}

TEST_F(RuntimeTest, SpawnBlockingDoesNotStarveWorkers) {
  // 1-worker runtime + many concurrent spawn_blocking tasks.  Without the
  // blocking-pool headroom, the single worker_main would occupy m_group's
  // only slot and the blocking submissions would dead-lock.
  auto             tiny = xpp::runtime::Runtime::new_multi_thread(1);
  constexpr int    kN = 8;
  std::atomic<int> counter{0};
  auto             orchestrate = [&]() -> xpp::Promise<int> {
    std::vector<xpp::Promise<int>> ps;
    ps.reserve(kN);
    for (int i = 0; i < kN; ++i) {
      ps.push_back(xpp::spawn_blocking([&counter, i] {
        counter.fetch_add(1, std::memory_order_relaxed);
        return i;
      }));
    }
    int sum = 0;
    for (auto &p : ps)
      sum += co_await p;
    co_return sum;
  };
  int sum = tiny->block_on(orchestrate);
  EXPECT_EQ(counter.load(), kN);
  EXPECT_EQ(sum, kN * (kN - 1) / 2);
}

/* ── Current-thread runtime ───────────────────────────────────────── */

class CurrentThreadRuntimeTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = xpp::runtime::Runtime::new_current_thread().into_raw();
  }
  void TearDown() override {
    delete m_rt;
  }
  xpp::runtime::Runtime *m_rt;
};

TEST_F(CurrentThreadRuntimeTest, BlockOnResolved) {
  EXPECT_EQ(m_rt->block_on(xpp::Promise<int>::resolve(42)), 42);
}

TEST_F(CurrentThreadRuntimeTest, BlockOnVoid) {
  m_rt->block_on(xpp::Promise<void>::resolve());
}

TEST_F(CurrentThreadRuntimeTest, BlockOnCoroutine) {
  auto coro = []() -> xpp::Promise<int> { co_return 99; };
  EXPECT_EQ(m_rt->block_on(coro()), 99);
}

TEST_F(CurrentThreadRuntimeTest, SpawnAndAwait) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto p      = m_rt->spawn([]() -> xpp::Promise<int> { co_return 7; });
    int  result = co_await p;
    co_return result;
  };
  EXPECT_EQ(m_rt->block_on(orchestrate()), 7);
}

TEST_F(CurrentThreadRuntimeTest, SpawnMultiple) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto h1 = m_rt->spawn([]() -> xpp::Promise<int> { co_return 9; });
    auto h2 = m_rt->spawn([]() -> xpp::Promise<int> { co_return 16; });
    int  a  = co_await h1;
    int  b  = co_await h2;
    co_return a + b;
  };
  EXPECT_EQ(m_rt->block_on(orchestrate()), 25);
}

TEST_F(CurrentThreadRuntimeTest, SpawnRunsOnCallingThread) {
  // Unlike multi-thread, current-thread tasks run on the block_on thread.
  auto main_tid = std::this_thread::get_id();
  auto orchestrate = [&]() -> xpp::Promise<bool> {
    auto p = m_rt->spawn(
      [&]() -> xpp::Promise<bool> { co_return std::this_thread::get_id() == main_tid; });
    co_return co_await p;
  };
  EXPECT_TRUE(m_rt->block_on(orchestrate()));
}

TEST_F(CurrentThreadRuntimeTest, SpawnVoidDetach) {
  std::atomic<int> counter{0};
  auto             orchestrate = [&]() -> xpp::Promise<void> {
    // Drop the handle: task is detached but still runs to completion
    // (it is drained by the current-thread loop before block_on returns).
    m_rt->spawn([&]() -> xpp::Promise<void> {
      counter.fetch_add(1);
      co_return;
    });
    co_return;
  };
  m_rt->block_on(orchestrate());
  // Give the detached task a drive turn.
  m_rt->block_on([]() -> xpp::Promise<void> { co_return; });
  EXPECT_EQ(counter.load(), 1);
}

TEST_F(CurrentThreadRuntimeTest, SpawnBlockingAwait) {
  auto orchestrate = [&]() -> xpp::Promise<int> {
    int v = co_await xpp::spawn_blocking([] { return 21; });
    co_return v * 2;
  };
  EXPECT_EQ(m_rt->block_on(orchestrate), 42);
}

TEST_F(CurrentThreadRuntimeTest, SpawnBlockingRunsOffThread) {
  auto main_tid = std::this_thread::get_id();
  std::thread::id blocking_tid{};
  auto            orchestrate = [&]() -> xpp::Promise<void> {
    co_await xpp::spawn_blocking([&] { blocking_tid = std::this_thread::get_id(); });
    co_return;
  };
  m_rt->block_on(orchestrate);
  EXPECT_NE(blocking_tid, std::thread::id{});
  EXPECT_NE(blocking_tid, main_tid);
}

/* ── Static factories ─────────────────────────────────────────────── */

TEST(RuntimeFactoryTest, MultiThreadBuildAndSpawn) {
  auto rt = xpp::runtime::Runtime::new_multi_thread(2, 64);

  auto orchestrate = [&]() -> xpp::Promise<int> {
    auto p = rt->spawn([]() -> xpp::Promise<int> { co_return 19; });
    co_return co_await p;
  };
  EXPECT_EQ(rt->block_on(orchestrate()), 19);
}

TEST(RuntimeFactoryTest, CurrentThreadBuildAndSpawn) {
  auto rt = xpp::runtime::Runtime::new_current_thread();

  auto orchestrate = [&]() -> xpp::Promise<int> {
    int  v = co_await xpp::spawn_blocking([] { return 11; });
    auto p = rt->spawn([]() -> xpp::Promise<int> { co_return 31; });
    co_return v + co_await p;
  };
  EXPECT_EQ(rt->block_on(orchestrate), 42);
}

TEST(RuntimeFactoryTest, DefaultWorkerThreads) {
  // 0 worker_threads => hardware concurrency; just verify it runs.
  auto rt = xpp::runtime::Runtime::new_multi_thread();
  EXPECT_EQ(rt->block_on(xpp::Promise<int>::resolve(5)), 5);
}

/* ── enter() ──────────────────────────────────────────────────────── */

TEST(RuntimeEnterTest, SpawnWithinEnterScope) {
  auto             rt = xpp::runtime::Runtime::new_multi_thread(2);
  std::atomic<int> n{0};
  {
    auto guard = rt->enter(); // guard returned by value
    EXPECT_EQ(xpp::runtime::Runtime::current(), rt.get());
    // Free xpp::spawn relies on the entered context; the detached task
    // runs on a worker even though nobody is driving block_on here.
    xpp::spawn([&]() -> xpp::Promise<void> {
      n.fetch_add(1);
      co_return;
    });
  }
  // Context restored on guard drop.
  EXPECT_EQ(xpp::runtime::Runtime::current(), nullptr);
  for (int i = 0; i < 2000 && n.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(n.load(), 1);
}

TEST(RuntimeEnterTest, GuardIsMovable) {
  auto rt = xpp::runtime::Runtime::new_multi_thread(1);
  auto g  = rt->enter();
  auto g2 = std::move(g); // move must not double-restore / dangle
  EXPECT_EQ(xpp::runtime::Runtime::current(), rt.get());
  (void)g2;
}

#else

#include <gtest/gtest.h>

TEST(RuntimeCoroTest, CoroutinesDisabled) {
  GTEST_SKIP() << "C++20 coroutines not available";
}

#endif // XPP_HAS_COROUTINES
