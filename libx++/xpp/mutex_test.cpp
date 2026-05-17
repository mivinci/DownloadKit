/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex_test.cpp - Tests for Mutex<T> and MutexGuard<T>.
 *
 * Single-thread:
 *   - lock returns a guard, unlock on dtor
 *   - try_lock yields Some when free, None when held
 *   - guard moves
 *   - operator-> / operator* / get expose the protected T
 *
 * Multi-thread:
 *   - Two threads racing to increment a shared counter end up at
 *     the right total (mutual exclusion holds).
 *
 * Condvar (wait / notify / wait_timeout) lives in cond_test.cpp.
 */

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <xpp/mutex.h>
#include <xpp/option.h>

namespace {

/* ── Single-thread basics ─────────────────────────────────────────── */

TEST(MutexTest, LockUnlockBalances) {
  xpp::Mutex<int> m(0);
  {
    auto g = m.lock();
    EXPECT_EQ(*g, 0);
    *g = 42;
  } // unlocked here
  {
    auto g = m.lock();
    EXPECT_EQ(*g, 42);
  }
}

TEST(MutexTest, GuardOperatorArrow) {
  xpp::Mutex<std::vector<int>> m;
  {
    auto g = m.lock();
    g->push_back(1);
    g->push_back(2);
    g->push_back(3);
    EXPECT_EQ(g->size(), 3u);
  }
  {
    auto g = m.lock();
    EXPECT_EQ((*g)[1], 2);
  }
}

TEST(MutexTest, GuardGetReturnsReference) {
  xpp::Mutex<int> m(7);
  auto            g    = m.lock();
  int            &lref = g.get();
  EXPECT_EQ(&lref, &(*g));
  lref = 99;
  EXPECT_EQ(*g, 99);
}

TEST(MutexTest, ForwardsCtorArguments) {
  // Variadic forwarding to T's ctor. std::string(n, ch) is a clean
  // multi-arg demo.
  xpp::Mutex<std::string> m(5, 'x'); // string(5, 'x') = "xxxxx"
  auto                    g = m.lock();
  EXPECT_EQ(*g, "xxxxx");
}

TEST(MutexTest, MoveGuardTransfersOwnership) {
  xpp::Mutex<int> m(0);
  auto            g  = m.lock();
  *g                 = 5;
  auto g2            = std::move(g);
  EXPECT_EQ(*g2, 5);
  // g is moved-from; only its dtor is safe
}

TEST(MutexTest, TryLockSucceedsWhenFree) {
  xpp::Mutex<int>                   m(11);
  xpp::Option<xpp::MutexGuard<int>> opt = m.try_lock();
  ASSERT_TRUE(opt.is_some());
  auto g = std::move(opt).unwrap();
  EXPECT_EQ(*g, 11);
}

TEST(MutexTest, TryLockFailsWhenHeld) {
  xpp::Mutex<int>                   m(0);
  auto                              g   = m.lock();
  xpp::Option<xpp::MutexGuard<int>> opt = m.try_lock();
  EXPECT_TRUE(opt.is_none()) << "try_lock should fail while another guard is alive";
}

/* ── Multi-thread: mutual exclusion ───────────────────────────────── */

TEST(MutexTest, MutualExclusionAcrossThreads) {
  constexpr int k_threads       = 8;
  constexpr int k_incs_per_thread = 5000;

  xpp::Mutex<int>          counter(0);
  std::vector<std::thread> ts;
  ts.reserve(k_threads);
  for (int i = 0; i < k_threads; ++i) {
    ts.emplace_back([&counter] {
      for (int j = 0; j < k_incs_per_thread; ++j) {
        auto g = counter.lock();
        *g     = *g + 1;
      }
    });
  }
  for (auto &t : ts) t.join();

  auto g = counter.lock();
  EXPECT_EQ(*g, k_threads * k_incs_per_thread);
}

} // namespace
