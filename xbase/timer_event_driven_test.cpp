/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer_event_driven_test.cpp - Unit tests for Plan C (timer-driven mode)
 */

#include <xbase/event.h>
#include <xbase/timer.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

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

/* ───────────────────── Attach / Detach basics ───────────────────── */

TEST(TimerDrivenAttach, AttachSuccess) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

TEST(TimerDrivenAttach, DoubleAttachFails) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);
  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Unknown);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

TEST(TimerDrivenAttach, NullArgsFail) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(NULL, loop), xErrno_Unknown);
  EXPECT_EQ(xTimerAttachEventLoop(t, NULL), xErrno_Unknown);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Timer fires after attach ───────────────────── */

TEST(TimerDrivenFire, TimerCallbackFires) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  std::atomic<int> fired{0};

  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &fired, 50);

  /* The timer thread is now driving xEventWait; poll mode callbacks
   * are enqueued to the MPSC queue. We need to call xTimerPoll(). */
  for (int i = 0; i < 40 && fired.load() == 0; i++) {
    sleep_ms(10);
    xTimerPoll(t);
  }

  EXPECT_GE(fired.load(), 1);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

/* ───────────────────── I/O events fire after attach ───────────────────── */

TEST(TimerDrivenIO, IOEventFires) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> io_count{0};

  xEventSource src = xEventAdd(loop, fds[0], xEvent_Read,
      [](int fd, xEventMask, void *arg) {
        static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        drain_fd(fd);
      }, &io_count);
  ASSERT_NE(src, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  /* Write data to trigger I/O */
  sleep_ms(20);
  write(fds[1], "hello", 5);

  for (int i = 0; i < 40 && io_count.load() == 0; i++)
    sleep_ms(10);

  EXPECT_GE(io_count.load(), 1);

  xEventDel(loop, src);
  xTimerDestroy(t);
  xEventLoopDestroy(loop);
  close(fds[0]);
  close(fds[1]);
}

/* ───────────────────── Mixed I/O + Timer ───────────────────── */

TEST(TimerDrivenMixed, IOAndTimerTogether) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> io_count{0};
  std::atomic<int> timer_count{0};

  xEventSource src = xEventAdd(loop, fds[0], xEvent_Read,
      [](int fd, xEventMask, void *arg) {
        static_cast<std::atomic<int> *>(arg)->fetch_add(1);
        drain_fd(fd);
      }, &io_count);
  ASSERT_NE(src, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &timer_count, 50);

  /* Write data at 30ms */
  sleep_ms(30);
  write(fds[1], "x", 1);

  for (int i = 0; i < 40 && (io_count.load() == 0 || timer_count.load() == 0); i++) {
    sleep_ms(10);
    xTimerPoll(t);
  }

  EXPECT_GE(io_count.load(), 1);
  EXPECT_GE(timer_count.load(), 1);

  xEventDel(loop, src);
  xTimerDestroy(t);
  xEventLoopDestroy(loop);
  close(fds[0]);
  close(fds[1]);
}

/* ───────────────────── Cross-thread submit wakes xEventWait ───────────────────── */

TEST(TimerDrivenCrossThread, SubmitWakesLoop) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  std::atomic<int> fired{0};

  /* Submit from main thread while timer thread is in xEventWait */
  sleep_ms(30);
  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &fired, 50);

  for (int i = 0; i < 40 && fired.load() == 0; i++) {
    sleep_ms(10);
    xTimerPoll(t);
  }

  EXPECT_GE(fired.load(), 1);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Cross-thread cancel wakes xEventWait ───────────────────── */

TEST(TimerDrivenCrossThread, CancelWakesLoop) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  std::atomic<int> fired{0};

  xTimerTask task = xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &fired, 500);
  ASSERT_NE(task, nullptr);

  sleep_ms(30);
  EXPECT_EQ(xTimerCancel(t, task), xErrno_Ok);

  /* Wait past original deadline */
  sleep_ms(100);
  xTimerPoll(t);

  EXPECT_EQ(fired.load(), 0);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Detach restores original behavior ───────────────────── */

TEST(TimerDrivenDetach, DetachRestoresBehavior) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  /* Detach */
  EXPECT_EQ(xTimerDetachEventLoop(t), xErrno_Ok);

  /* Timer should still work in original cond-wait mode */
  std::atomic<int> fired{0};

  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<int> *>(arg)->fetch_add(1);
  }, &fired, 50);

  for (int i = 0; i < 40 && fired.load() == 0; i++) {
    sleep_ms(10);
    xTimerPoll(t);
  }

  EXPECT_GE(fired.load(), 1);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

TEST(TimerDrivenDetach, DetachNotAttachedFails) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xTimerDetachEventLoop(t), xErrno_Unknown);

  xTimerDestroy(t);
}

TEST(TimerDrivenDetach, DetachNullFails) {
  EXPECT_EQ(xTimerDetachEventLoop(NULL), xErrno_Unknown);
}

/* ───────────────────── Destroy auto-detaches ───────────────────── */

TEST(TimerDrivenDestroy, DestroyAutoDetaches) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  /* Destroy should auto-detach and not crash */
  xTimerDestroy(t);

  /* Loop should still be usable */
  int n = xEventWait(loop, 10);
  EXPECT_EQ(n, 0);

  xEventLoopDestroy(loop);
}

/* ───────────────────── Timer precision ───────────────────── */

TEST(TimerDrivenPrecision, DelayAccuracy) {
  xTimer t = xTimerCreate(NULL);
  ASSERT_NE(t, nullptr);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xTimerAttachEventLoop(t, loop), xErrno_Ok);

  std::atomic<uint64_t> fire_time{0};

  uint64_t submit_time = xTimerNowMs();
  xTimerSubmitAfter(t, [](void *arg) {
    static_cast<std::atomic<uint64_t> *>(arg)->store(xTimerNowMs());
  }, &fire_time, 100);

  for (int i = 0; i < 60 && fire_time.load() == 0; i++) {
    sleep_ms(10);
    xTimerPoll(t);
  }

  uint64_t ft = fire_time.load();
  ASSERT_NE(ft, (uint64_t)0);

  int64_t delay = (int64_t)(ft - submit_time) - 100;
  EXPECT_GE(delay, -5);
  EXPECT_LE(delay, 50);

  xTimerDestroy(t);
  xEventLoopDestroy(loop);
}

/* ───────────────────── NULL safety ───────────────────── */

TEST(TimerDrivenNull, NullSafety) {
  /* These should not crash */
  xTimerAttachEventLoop(NULL, NULL);
  xTimerDetachEventLoop(NULL);
}
