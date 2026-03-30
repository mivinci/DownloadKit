/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer_test.cpp - Unit tests for xTimer
 */

#include <xbase/timer.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using ms = std::chrono::milliseconds;

static void sleep_ms(uint64_t n) {
  std::this_thread::sleep_for(ms(n));
}

/* ───────────────── xTimerNowMs ───────────────── */

TEST(TimerNowMs, Monotonic) {
  uint64_t a = xTimerNowMs();
  sleep_ms(10);
  uint64_t b = xTimerNowMs();
  EXPECT_GE(b, a + 5); /* at least 5 ms elapsed */
}

/* ───────────────── Create / Destroy ───────────────── */

TEST(TimerLifecycle, CreateAndDestroyEmpty) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);
  xTimerDestroy(t);
}

TEST(TimerLifecycle, CreateWithTaskGroup) {
  xTaskGroup g = xTaskGroupCreate(NULL);
  ASSERT_NE(g, nullptr);
  xTimer t = xTimerCreate(g);
  ASSERT_NE(t, nullptr);
  xTimerDestroy(t);
  xTaskGroupDestroy(g);
}

TEST(TimerLifecycle, DestroyWithPendingTasks) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);
  /* Submit tasks that won't fire before destroy */
  xTimerSubmitAfter(t, [](void *) {}, nullptr, 10000);
  xTimerSubmitAfter(t, [](void *) {}, nullptr, 20000);
  /* Should not crash or leak */
  xTimerDestroy(t);
}

/* ───────────────── SubmitAfter (direct execution) ───────────────── */

TEST(TimerSubmitAfter, FiresAfterDelay) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  std::atomic<int> fired{0};
  uint64_t start = xTimerNowMs();
  uint64_t fire_at = 0;

  xTimerSubmitAfter(t, [](void *arg) {
    auto *p = static_cast<std::pair<std::atomic<int>*, uint64_t*>*>(arg);
    p->second[0] = xTimerNowMs();
    p->first->store(1);
  }, new std::pair<std::atomic<int>*, uint64_t*>(&fired, &fire_at), 80);

  /* Wait up to 500 ms */
  for (int i = 0; i < 50 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_EQ(fired.load(), 1);
  EXPECT_GE(fire_at, start + 70); /* fired at least ~80ms later */

  xTimerDestroy(t);
}

TEST(TimerSubmitAfter, ZeroDelayFiresImmediately) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  std::atomic<int> fired{0};
  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int>*>(arg)->store(1);
  }, &fired, 0);

  for (int i = 0; i < 20 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_EQ(fired.load(), 1);
  xTimerDestroy(t);
}

TEST(TimerSubmitAfter, MultipleTasksOrdering) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  std::vector<int> order;
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

  struct Ctx { std::vector<int> *order; pthread_mutex_t *mu; int id; };

  auto cb = [](void *arg) {
    auto *ctx = static_cast<Ctx*>(arg);
    pthread_mutex_lock(ctx->mu);
    ctx->order->push_back(ctx->id);
    pthread_mutex_unlock(ctx->mu);
    delete ctx;
  };

  xTimerSubmitAfter(t, cb, new Ctx{&order, &mu, 3}, 90);
  xTimerSubmitAfter(t, cb, new Ctx{&order, &mu, 1}, 30);
  xTimerSubmitAfter(t, cb, new Ctx{&order, &mu, 2}, 60);

  sleep_ms(300);

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);

  xTimerDestroy(t);
}

/* ───────────────── SubmitAt ───────────────── */

TEST(TimerSubmitAt, FiresAtAbsoluteTime) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  std::atomic<int> fired{0};
  uint64_t target = xTimerNowMs() + 80;

  xTimerSubmitAt(t, [](void *arg) {
    static_cast<std::atomic<int>*>(arg)->store(1);
  }, &fired, target);

  for (int i = 0; i < 50 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_EQ(fired.load(), 1);
  EXPECT_GE(xTimerNowMs(), target);

  xTimerDestroy(t);
}

TEST(TimerSubmitAt, PastDeadlineFiresImmediately) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  std::atomic<int> fired{0};
  uint64_t past = xTimerNowMs() - 1000; /* 1 s in the past */

  xTimerSubmitAt(t, [](void *arg) {
    static_cast<std::atomic<int>*>(arg)->store(1);
  }, &fired, past);

  for (int i = 0; i < 20 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_EQ(fired.load(), 1);
  xTimerDestroy(t);
}

/* ───────────────── Cancel ───────────────── */

TEST(TimerCancel, CancelBeforeFire) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  std::atomic<int> fired{0};
  xTimerTask task = xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int>*>(arg)->store(1);
  }, &fired, 200);

  ASSERT_NE(task, nullptr);
  xErrno err = xTimerCancel(t, task);
  EXPECT_EQ(err, xErrno_Ok);

  sleep_ms(400);
  EXPECT_EQ(fired.load(), 0); /* must not have fired */

  xTimerDestroy(t);
}

TEST(TimerCancel, NullArguments) {
  EXPECT_EQ(xTimerCancel(NULL, NULL), xErrno_Unknown);
}

/* ───────────────── With TaskGroup ───────────────── */

TEST(TimerWithGroup, CallbackRunsOnWorkerThread) {
  xTaskGroupConf conf = {2, 0};
  xTaskGroup g = xTaskGroupCreate(&conf);
  ASSERT_NE(g, nullptr);

  xTimer t = xTimerCreate(g);
  ASSERT_NE(t, nullptr);

  std::atomic<int> fired{0};
  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int>*>(arg)->fetch_add(1);
  }, &fired, 50);

  sleep_ms(300);
  EXPECT_EQ(fired.load(), 1);

  xTimerDestroy(t);
  xTaskGroupDestroy(g);
}

TEST(TimerWithGroup, ManyTasksWithGroup) {
  xTaskGroup g = xTaskGroupCreate(NULL);
  ASSERT_NE(g, nullptr);

  xTimer t = xTimerCreate(g);
  ASSERT_NE(t, nullptr);

  const int N = 20;
  std::atomic<int> counter{0};

  for (int i = 0; i < N; i++) {
    xTimerSubmitAfter(t, [](void *arg) {
      static_cast<std::atomic<int>*>(arg)->fetch_add(1);
    }, &counter, (uint64_t)(i * 10));
  }

  sleep_ms(500);
  EXPECT_EQ(counter.load(), N);

  xTimerDestroy(t);
  xTaskGroupDestroy(g);
}
