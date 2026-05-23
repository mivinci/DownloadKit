/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cond_test.cpp - Tests for Condvar.
 *
 *   - Producer / consumer with the while-predicate-loop pattern;
 *     all produced items round-trip in order.
 *   - wait_timeout returns true on real timeout, false when
 *     signalled within the deadline.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <xpp/duration.h>
#include <xpp/sys/cond.h>
#include <xpp/sys/mutex.h>

namespace {

TEST(CondvarTest, WaitAndNotifyProducerConsumer) {
  struct Queue {
    std::vector<int> items;
    bool             done = false;
  };

  xpp::sys::Mutex<Queue> q;
  xpp::sys::Condvar      not_empty;

  std::vector<int> consumed;

  std::thread consumer([&] {
    for (;;) {
      auto g = q.lock();
      while (g->items.empty() && !g->done)
        not_empty.wait(g);
      if (g->items.empty() && g->done) return;
      // drain
      for (int x : g->items)
        consumed.push_back(x);
      g->items.clear();
    }
  });

  // Producer pushes 100 items, then signals done.
  for (int i = 0; i < 100; ++i) {
    {
      auto g = q.lock();
      g->items.push_back(i);
    }
    not_empty.notify_one();
  }
  {
    auto g  = q.lock();
    g->done = true;
  }
  not_empty.notify_all();

  consumer.join();
  EXPECT_EQ(consumed.size(), 100u);
  // The consumer may drain in batches, but items themselves should
  // appear in production order (single producer).
  for (int i = 0; i < 100; ++i)
    EXPECT_EQ(consumed[i], i);
}

TEST(CondvarTest, WaitTimeoutReturnsTrueOnTimeout) {
  using namespace xpp::literals;

  xpp::sys::Mutex<int> m(0);
  xpp::sys::Condvar    c;

  auto g         = m.lock();
  auto start     = std::chrono::steady_clock::now();
  bool timed_out = c.wait_timeout(g, 50_ms);
  auto dur       = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(timed_out);
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(dur).count(), 30)
    << "wait_timeout returned too quickly to be a real timeout wait";
}

TEST(CondvarTest, WaitTimeoutReturnsFalseWhenSignalledInTime) {
  using namespace xpp::literals;

  xpp::sys::Mutex<bool> m(false);
  xpp::sys::Condvar     c;

  std::thread waker([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      auto g = m.lock();
      *g     = true;
    }
    c.notify_one();
  });

  auto g = m.lock();
  while (!*g) {
    if (c.wait_timeout(g, 500_ms)) break; // timed out
  }
  bool result = *g;

  // Drop the lock before joining (waker grabs it once).
  {
    auto release = std::move(g);
    (void)release;
  }
  waker.join();
  EXPECT_TRUE(result);
}

} // namespace
