/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_timer_attach_test.cpp - Integration tests for Plan A (attach mode)
 */

#include <xbase/event.h>
#include <xbase/timer.h>
#include <xbase/task.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

static int make_pipe(int fds[2]) {
  if (pipe(fds) != 0) return -1;
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
  return 0;
}

static void drain_fd(int fd) {
  char buf[256];
  while (read(fd, buf, sizeof(buf)) > 0)
    ;
}

/* ───────────────────── xTimerNextDeadline ───────────────────── */

TEST(TimerNextDeadline, NullReturnsMax) {
  EXPECT_EQ(xTimerNextDeadline(NULL), UINT64_MAX);
}

TEST(TimerNextDeadline, EmptyHeapReturnsMax) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xTimerNextDeadline(t), UINT64_MAX);

  xTimerDestroy(t);
}

TEST(TimerNextDeadline, ReturnsCorrectDeadline) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  /* Submit a timer 500ms from now */
  uint64_t before = xTimerNowMs();
  xTimerTask task = xTimerSubmitAfter(t, [](void *) {}, nullptr, 500);
  ASSERT_NE(task, nullptr);

  uint64_t deadline = xTimerNextDeadline(t);
  uint64_t after = xTimerNowMs();

  /* Deadline should be approximately before + 500 */
  EXPECT_GE(deadline, before + 490);
  EXPECT_LE(deadline, after + 510);

  xTimerCancel(t, task);
  xTimerDestroy(t);
}

TEST(TimerNextDeadline, ReflectsAfterCancel) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xTimerTask task1 = xTimerSubmitAfter(t, [](void *) {}, nullptr, 100);
  xTimerTask task2 = xTimerSubmitAfter(t, [](void *) {}, nullptr, 500);
  ASSERT_NE(task1, nullptr);
  ASSERT_NE(task2, nullptr);

  /* Deadline should be ~100ms from now (task1 is earlier) */
  uint64_t d1 = xTimerNextDeadline(t);

  /* Cancel the earlier task */
  xTimerCancel(t, task1);

  /* Now deadline should be ~500ms from now (task2) */
  uint64_t d2 = xTimerNextDeadline(t);
  EXPECT_GT(d2, d1 + 300);

  xTimerCancel(t, task2);
  xTimerDestroy(t);
}

/* ───────────────────── xTimerIsPollMode ───────────────────── */

TEST(TimerIsPollMode, NullReturnsFalse) {
  EXPECT_EQ(xTimerIsPollMode(NULL), 0);
}

TEST(TimerIsPollMode, PollModeReturnsTrue) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xTimerIsPollMode(t), 1);
  xTimerDestroy(t);
}

TEST(TimerIsPollMode, PushModeReturnsFalse) {
  xTaskGroup g = xTaskGroupCreate(NULL);
  ASSERT_NE(g, nullptr);

  xTimer t = xTimerCreate(g);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xTimerIsPollMode(t), 0);

  xTimerDestroy(t);
  xTaskGroupDestroy(g);
}

/* ───────────────────── xEventLoopCreate with timer ───────────────────── */

TEST(EventLoopCreateTimer, NullTimerBackwardCompat) {
  xEventLoop loop = xEventLoopCreate(NULL);
  ASSERT_NE(loop, nullptr);
  xEventLoopDestroy(loop);
}

TEST(EventLoopCreateTimer, PollModeTimerSuccess) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate(t);
  ASSERT_NE(loop, nullptr);

  xEventLoopDestroy(loop);
  xTimerDestroy(t);
}

TEST(EventLoopCreateTimer, PushModeTimerFails) {
  xTaskGroup g = xTaskGroupCreate(NULL);
  ASSERT_NE(g, nullptr);

  xTimer t = xTimerCreate(g);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate(t);
  EXPECT_EQ(loop, nullptr);

  xTimerDestroy(t);
  xTaskGroupDestroy(g);
}

/* ───────────────────── xEventLoopRun / xEventLoopStop ───────────────────── */

TEST(EventLoopRun, NullDoesNotCrash) {
  xEventLoopRun(NULL);
  xEventLoopStop(NULL);
}

TEST(EventLoopRun, StopTerminatesLoop) {
  xEventLoop loop = xEventLoopCreate(NULL);
  ASSERT_NE(loop, nullptr);

  std::atomic<bool> running{false};

  std::thread runner([&]() {
    running = true;
    xEventLoopRun(loop);
  });

  /* Wait for the loop to start */
  while (!running) sleep_ms(1);
  sleep_ms(20);

  xEventLoopStop(loop);
  runner.join(); /* Should return promptly */

  xEventLoopDestroy(loop);
}

TEST(EventLoopRun, IOOnlyWithoutTimer) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate(NULL);
  ASSERT_NE(loop, nullptr);

  std::atomic<int> count{0};

  xEventSource src = xEventAdd(loop, fds[0], xEvent_Read,
      [](int fd, xEventMask, void *arg) {
        static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        drain_fd(fd);
      }, &count);
  ASSERT_NE(src, nullptr);

  std::thread runner([&]() {
    xEventLoopRun(loop);
  });

  sleep_ms(30);
  write(fds[1], "x", 1);
  sleep_ms(50);

  EXPECT_GE(count.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close(fds[0]);
  close(fds[1]);
}

TEST(EventLoopRun, TimerCallbackFires) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate(t);
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &fired, 50);

  std::thread runner([&]() {
    xEventLoopRun(loop);
  });

  /* Wait for timer to fire */
  for (int i = 0; i < 40 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_GE(fired.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopDestroy(loop);
  xTimerDestroy(t);
}

TEST(EventLoopRun, MixedIOAndTimer) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate(t);
  ASSERT_NE(loop, nullptr);

  std::atomic<int> io_count{0};
  std::atomic<int> timer_count{0};

  xEventSource src = xEventAdd(loop, fds[0], xEvent_Read,
      [](int fd, xEventMask, void *arg) {
        static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        drain_fd(fd);
      }, &io_count);
  ASSERT_NE(src, nullptr);

  /* Schedule a timer at 50ms */
  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &timer_count, 50);

  std::thread runner([&]() {
    xEventLoopRun(loop);
  });

  /* Write data at 30ms to trigger I/O */
  sleep_ms(30);
  write(fds[1], "hello", 5);

  /* Wait for both to fire */
  for (int i = 0; i < 40 && (io_count.load() == 0 || timer_count.load() == 0); i++)
    sleep_ms(10);

  EXPECT_GE(io_count.load(), 1);
  EXPECT_GE(timer_count.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  xTimerDestroy(t);
  close(fds[0]);
  close(fds[1]);
}

TEST(EventLoopRun, TimerPrecision) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate(t);
  ASSERT_NE(loop, nullptr);

  std::atomic<uint64_t> fire_time{0};

  uint64_t submit_time = xTimerNowMs();
  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<uint64_t> *>(arg)->store(xTimerNowMs());
  }, &fire_time, 100);

  std::thread runner([&]() {
    xEventLoopRun(loop);
  });

  /* Wait for timer to fire */
  for (int i = 0; i < 60 && fire_time.load() == 0; i++)
    sleep_ms(10);

  uint64_t ft = fire_time.load();
  ASSERT_NE(ft, (uint64_t)0);

  int64_t delay = (int64_t)(ft - submit_time) - 100;
  EXPECT_GE(delay, -5);   /* should not fire early */
  EXPECT_LE(delay, 50);   /* should not be too late */

  xEventLoopStop(loop);
  runner.join();

  xEventLoopDestroy(loop);
  xTimerDestroy(t);
}

TEST(EventLoopRun, MultipleTimersInOrder) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate(t);
  ASSERT_NE(loop, nullptr);

  std::vector<int> order;
  std::mutex mu;

  auto record = [](void *arg) {
    auto *ctx = static_cast<std::pair<std::vector<int> *, std::mutex *> *>(arg);
    std::lock_guard<std::mutex> lock(*ctx->second);
    ctx->first->push_back(
        static_cast<int>(ctx->first->size()));
  };

  /* Use pairs to pass context - each timer records its index */
  struct TimerCtx {
    std::vector<int> *order;
    std::mutex *mu;
    int id;
  };

  TimerCtx ctx1{&order, &mu, 1};
  TimerCtx ctx2{&order, &mu, 2};
  TimerCtx ctx3{&order, &mu, 3};

  auto timer_fn = [](void *arg) {
    auto *ctx = static_cast<TimerCtx *>(arg);
    std::lock_guard<std::mutex> lock(*ctx->mu);
    ctx->order->push_back(ctx->id);
  };

  /* Submit in reverse order of deadline */
  xTimerSubmitAfter(t, timer_fn, &ctx3, 150);
  xTimerSubmitAfter(t, timer_fn, &ctx1, 50);
  xTimerSubmitAfter(t, timer_fn, &ctx2, 100);

  std::thread runner([&]() {
    xEventLoopRun(loop);
  });

  /* Wait for all 3 timers */
  for (int i = 0; i < 60; i++) {
    sleep_ms(10);
    std::lock_guard<std::mutex> lock(mu);
    if (order.size() >= 3) break;
  }

  xEventLoopStop(loop);
  runner.join();

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);

  xEventLoopDestroy(loop);
  xTimerDestroy(t);
}
