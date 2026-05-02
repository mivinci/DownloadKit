/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_timer_builtin_test.cpp - Unit tests for Plan B (builtin timer)
 */

#include <xbase/event.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

static int make_pipe(int fds[2]) {
#ifdef _WIN32
  static bool wsa_init = false;
  if (!wsa_init) {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    wsa_init = true;
  }
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(listener); return -1;
  }
  if (listen(listener, 1) != 0) { closesocket(listener); return -1; }
  int addrlen = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) {
    closesocket(listener); return -1;
  }
  SOCKET conn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn == INVALID_SOCKET) { closesocket(listener); return -1; }
  if (connect(conn, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(listener); closesocket(conn); return -1;
  }
  SOCKET acceptor = accept(listener, NULL, NULL);
  closesocket(listener);
  if (acceptor == INVALID_SOCKET) { closesocket(conn); return -1; }
  u_long mode = 1;
  ioctlsocket(acceptor, FIONBIO, &mode);
  ioctlsocket(conn, FIONBIO, &mode);
  fds[0] = (int)acceptor;
  fds[1] = (int)conn;
  return 0;
#else
  if (pipe(fds) != 0) return -1;
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
  return 0;
#endif
}

static void close_fd(int fd) {
#ifdef _WIN32
  closesocket((SOCKET)fd);
#else
  close(fd);
#endif
}

static void drain_fd(int fd) {
  char buf[256];
#ifdef _WIN32
  while (recv((SOCKET)fd, buf, sizeof(buf), 0) > 0)
    ;
#else
  while (read(fd, buf, sizeof(buf)) > 0)
    ;
#endif
}

static void write_fd(int fd, const char *data, size_t len) {
#ifdef _WIN32
  send((SOCKET)fd, data, (int)len, 0);
#else
  write(fd, data, len);
#endif
}

/* ───────────────────── TimerAfter ───────────────────── */

TEST(BuiltinTimerAfter, BasicDelay) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xEventTimer t = xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 50);
  ASSERT_NE(t, nullptr);

  /* Wait long enough for the timer to fire (may need multiple waits
   * because the initial xEventWake from TimerAfter can return early) */
  for (int i = 0; i < 10 && fired.load() == 0; i++)
    xEventWait(loop, 50);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAfter, ZeroDelayFiresImmediately) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 0);

  xEventWait(loop, 100);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAfter, NullArgs) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xEventLoopTimerAfter(NULL, [](void *) {}, nullptr, 100), nullptr);
  EXPECT_EQ(xEventLoopTimerAfter(loop, NULL, nullptr, 100), nullptr);

  xEventLoopDestroy(loop);
}

/* ───────────────────── TimerAt ───────────────────── */

TEST(BuiltinTimerAt, AbsoluteTime) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  uint64_t    deadline = xEventLoopNowMs() + 50;
  xEventTimer t        = xEventLoopTimerAt(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, deadline);
  ASSERT_NE(t, nullptr);

  for (int i = 0; i < 10 && fired.load() == 0; i++)
    xEventWait(loop, 50);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerAt, ExpiredDeadlineFiresImmediately) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  /* Deadline in the past */
  xEventLoopTimerAt(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 0);

  xEventWait(loop, 100);

  EXPECT_EQ(fired.load(), 1);

  xEventLoopDestroy(loop);
}

/* ───────────────────── TimerCancel ───────────────────── */

TEST(BuiltinTimerCancel, CancelBeforeFire) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xEventTimer t = xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 500);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xEventLoopTimerCancel(loop, t), xErrno_Ok);

  /* Wait past the original deadline */
  xEventWait(loop, 100);

  EXPECT_EQ(fired.load(), 0);

  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerCancel, NullArgs) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xEventLoopTimerCancel(NULL, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xEventLoopTimerCancel(loop, NULL), xErrno_InvalidArg);

  xEventLoopDestroy(loop);
}

/* ───────────────────── Multiple timers ordering ───────────────────── */

TEST(BuiltinTimerOrder, MultipleTimersInOrder) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::vector<int> order;
  std::mutex       mu;

  struct Ctx {
    std::vector<int> *order;
    std::mutex       *mu;
    int               id;
  };

  auto fn = [](void *arg) {
    auto                       *ctx = static_cast<Ctx *>(arg);
    std::lock_guard<std::mutex> lock(*ctx->mu);
    ctx->order->push_back(ctx->id);
  };

  Ctx ctx1{&order, &mu, 1};
  Ctx ctx2{&order, &mu, 2};
  Ctx ctx3{&order, &mu, 3};

  xEventLoopTimerAfter(loop, fn, &ctx3, 150);
  xEventLoopTimerAfter(loop, fn, &ctx1, 50);
  xEventLoopTimerAfter(loop, fn, &ctx2, 100);

  /* Wait for all to fire */
  for (int i = 0; i < 10; i++) {
    xEventWait(loop, 50);
    std::lock_guard<std::mutex> lock(mu);
    if (order.size() >= 3) break;
  }

  std::lock_guard<std::mutex> lock(mu);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);

  xEventLoopDestroy(loop);
}

/* ───────────────────── Mixed I/O + Timer ───────────────────── */

TEST(BuiltinTimerMixed, IOAndTimerTogether) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> io_count{0};
  std::atomic<int> timer_count{0};

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      static_cast<std::atomic<int> *>(arg)->fetch_add(1);
      drain_fd(fd);
    },
    &io_count);
  ASSERT_NE(src, nullptr);

  xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &timer_count, 50);

  /* Write data to trigger I/O */
  write_fd(fds[1], "x", 1);

  /* Wait for both */
  for (int i = 0; i < 10; i++) {
    xEventWait(loop, 50);
    if (io_count.load() >= 1 && timer_count.load() >= 1) break;
  }

  EXPECT_GE(io_count.load(), 1);
  EXPECT_GE(timer_count.load(), 1);

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Run + Stop ───────────────────── */

TEST(BuiltinTimerRun, RunAndStop) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<bool> running{false};

  std::thread runner([&]() {
    running = true;
    xEventLoopRun(loop);
  });

  while (!running)
    sleep_ms(1);
  sleep_ms(20);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopDestroy(loop);
}

TEST(BuiltinTimerRun, NullDoesNotCrash) {
  xEventLoopRun(NULL);
  xEventLoopStop(NULL);
}

TEST(BuiltinTimerRun, TimerFiresDuringRun) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 50);

  std::thread runner([&]() { xEventLoopRun(loop); });

  for (int i = 0; i < 40 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_GE(fired.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopDestroy(loop);
}

/* ───────────────────── Cross-thread timer submit ───────────────────── */

TEST(BuiltinTimerCrossThread, SubmitFromAnotherThread) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  std::thread runner([&]() { xEventLoopRun(loop); });

  sleep_ms(20);

  /* Submit timer from a different thread */
  xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 50);

  for (int i = 0; i < 40 && fired.load() == 0; i++)
    sleep_ms(10);

  EXPECT_GE(fired.load(), 1);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopDestroy(loop);
}

/* ───────────────────── Destroy discards pending timers ─────────────────────
 */

TEST(BuiltinTimerDestroy, DiscardsPendingTimers) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  /* Schedule timers far in the future */
  xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 10000);
  xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 20000);

  /* Destroy without waiting — should not crash or fire callbacks */
  xEventLoopDestroy(loop);

  EXPECT_EQ(fired.load(), 0);
}

/* ───────────────────── Timer precision ───────────────────── */

TEST(BuiltinTimerPrecision, DelayAccuracy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<uint64_t> fire_time{0};

  uint64_t submit_time = xEventLoopNowMs();
  xEventLoopTimerAfter(
    loop,
    [](void *arg) {
      static_cast<std::atomic<uint64_t> *>(arg)->store(xEventLoopNowMs());
    },
    &fire_time, 100);

  std::thread runner([&]() { xEventLoopRun(loop); });

  for (int i = 0; i < 60 && fire_time.load() == 0; i++)
    sleep_ms(10);

  uint64_t ft = fire_time.load();
  ASSERT_NE(ft, (uint64_t)0);

  int64_t delay = (int64_t)(ft - submit_time) - 100;
  EXPECT_GE(delay, -5);
  EXPECT_LE(delay, 50);

  xEventLoopStop(loop);
  runner.join();

  xEventLoopDestroy(loop);
}

/* ───────────────────── Cancel after fire ───────────────────── */

TEST(BuiltinTimerCancel, CancelAfterFireReturnsError) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xEventTimer t = xEventLoopTimerAfter(
    loop, [](void *arg) { static_cast<std::atomic<int> *>(arg)->fetch_add(1); },
    &fired, 10);
  ASSERT_NE(t, nullptr);

  /* Wait for it to fire */
  for (int i = 0; i < 20 && fired.load() == 0; i++)
    xEventWait(loop, 20);

  EXPECT_EQ(fired.load(), 1);

  /* Cancel after fire should return InvalidState */
  xErrno err = xEventLoopTimerCancel(loop, t);
  EXPECT_EQ(err, xErrno_InvalidState);

  xEventLoopDestroy(loop);
}

/* ───────────────────── NowMs basic test ───────────────────── */

TEST(BuiltinTimerNowMs, ReturnsNonZero) {
  uint64_t now = xEventLoopNowMs();
  EXPECT_GT(now, 0u);
}

TEST(BuiltinTimerNowMs, IsMonotonic) {
  uint64_t a = xEventLoopNowMs();
  sleep_ms(10);
  uint64_t b = xEventLoopNowMs();
  EXPECT_GE(b, a);
}
