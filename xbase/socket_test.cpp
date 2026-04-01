/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket_test.cpp - Unit tests for xSocket
 */

#include <xbase/socket.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void noop_callback(xSocket, xEventMask, void *) {}

static void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

/* Drain all data from a non-blocking fd. */
static void drain_fd(int fd) {
  char buf[256];
  while (read(fd, buf, sizeof(buf)) > 0)
    ;
}

/*
 * Repeatedly call xEventWait until total_ms has elapsed.
 * This is needed because xEventLoopTimerAfter wakes the loop,
 * causing the first xEventWait to return immediately before the
 * timer deadline.
 */
static void pump_loop(xEventLoop loop, int total_ms) {
  auto deadline = std::chrono::steady_clock::now() + ms(total_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    auto remaining = std::chrono::duration_cast<ms>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) break;
    xEventWait(loop, (int)remaining);
  }
}

/* ───────────────────── SocketCreate ───────────────────── */

TEST(SocketCreate, Success) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  EXPECT_GE(fd, 0);

  /* Verify O_NONBLOCK */
  int flags = fcntl(fd, F_GETFL, 0);
  EXPECT_TRUE(flags & O_NONBLOCK);

  /* Verify FD_CLOEXEC */
  int fdflags = fcntl(fd, F_GETFD, 0);
  EXPECT_TRUE(fdflags & FD_CLOEXEC);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketCreate, NullLoop) {
  xSocket sock = xSocketCreate(NULL, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read, noop_callback, nullptr);
  EXPECT_EQ(sock, nullptr);
}

TEST(SocketCreate, NullCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read, NULL, nullptr);
  EXPECT_EQ(sock, nullptr);

  xEventLoopDestroy(loop);
}

TEST(SocketCreate, InvalidFamily) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  /* Use an invalid address family to trigger socket() failure */
  xSocket sock = xSocketCreate(loop, -1, SOCK_STREAM, 0,
                                xEvent_Read, noop_callback, nullptr);
  EXPECT_EQ(sock, nullptr);

  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketDestroy ───────────────────── */

TEST(SocketDestroy, Normal) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  ASSERT_GE(fd, 0);

  xSocketDestroy(loop, sock);

  /* Verify fd is closed: fcntl on a closed fd should fail */
  EXPECT_EQ(fcntl(fd, F_GETFD), -1);

  xEventLoopDestroy(loop);
}

TEST(SocketDestroy, Null) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  /* Must not crash */
  xSocketDestroy(loop, NULL);

  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketMask (SetMask) ───────────────────── */

TEST(SocketMask, SetAndGet) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  EXPECT_EQ(xSocketMask(sock), xEvent_Read);

  xErrno err = xSocketSetMask(loop, sock, xEvent_Write);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xSocketMask(sock), xEvent_Write);

  err = xSocketSetMask(loop, sock, (xEventMask)(xEvent_Read | xEvent_Write));
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xSocketMask(sock), (xEventMask)(xEvent_Read | xEvent_Write));

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketMask, InvalidHandle) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(xSocketSetMask(loop, NULL, xEvent_Read), xErrno_InvalidArg);

  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketQuery ───────────────────── */

TEST(SocketQuery, Fd) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  int fd = xSocketFd(sock);
  EXPECT_GE(fd, 0);

  /* Verify it's a valid socket fd */
  int optval;
  socklen_t optlen = sizeof(optval);
  EXPECT_EQ(getsockopt(fd, SOL_SOCKET, SO_TYPE, &optval, &optlen), 0);
  EXPECT_EQ(optval, SOCK_STREAM);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketQuery, Mask) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Write, noop_callback, nullptr);
  ASSERT_NE(sock, nullptr);

  EXPECT_EQ(xSocketMask(sock), xEvent_Write);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketQuery, NullFd) {
  EXPECT_EQ(xSocketFd(NULL), -1);
}

TEST(SocketQuery, NullMask) {
  EXPECT_EQ(xSocketMask(NULL), 0);
}

/* ───────────────────── SocketTimeout ───────────────────── */

TEST(SocketTimeout, ReadTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  struct Ctx {
    xSocket      sock;
    xEventMask   mask;
    int          count;
  } ctx = {nullptr, 0, 0};

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket s, xEventMask m, void *arg) {
                                  auto *c = static_cast<Ctx *>(arg);
                                  c->sock  = s;
                                  c->mask  = m;
                                  c->count++;
                                }, &ctx);
  ASSERT_NE(sock, nullptr);
  ctx.sock = nullptr;

  /* Set a short read timeout */
  xErrno err = xSocketSetTimeout(sock, 50, 0);
  EXPECT_EQ(err, xErrno_Ok);

  /* Pump the loop until the timeout fires */
  pump_loop(loop, 200);

  EXPECT_GE(ctx.count, 1);
  EXPECT_TRUE(ctx.mask & xEvent_Timeout);
  EXPECT_EQ(ctx.sock, sock);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, WriteTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  struct Ctx {
    xEventMask   mask;
    int          timeout_count;
  } ctx = {0, 0};

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask m, void *arg) {
                                  auto *c = static_cast<Ctx *>(arg);
                                  if (m & xEvent_Timeout) {
                                    c->mask = m;
                                    c->timeout_count++;
                                  }
                                }, &ctx);
  ASSERT_NE(sock, nullptr);

  /* Set a short write timeout */
  xErrno err = xSocketSetTimeout(sock, 0, 50);
  EXPECT_EQ(err, xErrno_Ok);

  /* Pump the loop until the timeout fires */
  pump_loop(loop, 200);

  EXPECT_GE(ctx.timeout_count, 1);
  EXPECT_TRUE(ctx.mask & xEvent_Timeout);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, IdleReset) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask m, void *arg) {
                                  if (m & xEvent_Timeout)
                                    (*static_cast<int *>(arg))++;
                                }, &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set a 100ms read timeout */
  xSocketSetTimeout(sock, 100, 0);

  /* Pump for 50ms — timeout should not have fired yet */
  pump_loop(loop, 50);
  EXPECT_EQ(timeout_count, 0);

  /* Reset the timer by calling SetTimeout again (simulates idle reset) */
  xSocketSetTimeout(sock, 100, 0);

  /* Pump for 80ms — the reset timer should NOT have fired yet */
  pump_loop(loop, 80);
  EXPECT_EQ(timeout_count, 0);

  /* Pump for another 50ms — now the reset timer (100ms from reset) should fire */
  pump_loop(loop, 50);
  EXPECT_EQ(timeout_count, 1);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, CancelWithZero) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask m, void *arg) {
                                  if (m & xEvent_Timeout)
                                    (*static_cast<int *>(arg))++;
                                }, &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set a 80ms read timeout, then cancel it */
  xSocketSetTimeout(sock, 80, 0);
  xSocketSetTimeout(sock, 0, 0);

  /* Pump long enough for the original timeout to have fired */
  pump_loop(loop, 150);
  EXPECT_EQ(timeout_count, 0);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, ReplaceTimeout) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask m, void *arg) {
                                  if (m & xEvent_Timeout)
                                    (*static_cast<int *>(arg))++;
                                }, &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set a 50ms read timeout, then replace with 200ms */
  xSocketSetTimeout(sock, 50, 0);
  xSocketSetTimeout(sock, 200, 0);

  /* Pump 100ms — the original 50ms timeout should NOT fire (replaced) */
  pump_loop(loop, 100);
  EXPECT_EQ(timeout_count, 0);

  /* Pump another 150ms — the 200ms timeout should fire */
  pump_loop(loop, 150);
  EXPECT_EQ(timeout_count, 1);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketTimeout, DestroyCancel) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int timeout_count = 0;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask m, void *arg) {
                                  if (m & xEvent_Timeout)
                                    (*static_cast<int *>(arg))++;
                                }, &timeout_count);
  ASSERT_NE(sock, nullptr);

  /* Set timeouts */
  xSocketSetTimeout(sock, 80, 80);

  /* Destroy before timeouts fire */
  xSocketDestroy(loop, sock);

  /* Pump long enough — timeouts should NOT fire (cancelled by destroy) */
  pump_loop(loop, 200);
  EXPECT_EQ(timeout_count, 0);

  xEventLoopDestroy(loop);
}

/* ───────────────────── SocketCallback ───────────────────── */

TEST(SocketCallback, HandleMatch) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xSocket received_sock = nullptr;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket s, xEventMask, void *arg) {
                                  *static_cast<xSocket *>(arg) = s;
                                }, &received_sock);
  ASSERT_NE(sock, nullptr);

  /* Use timeout to trigger the callback */
  xSocketSetTimeout(sock, 50, 0);
  pump_loop(loop, 200);

  EXPECT_EQ(received_sock, sock);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketCallback, UserpMatch) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int sentinel = 42;
  int *received_userp = nullptr;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask, void *arg) {
                                  /* arg IS the userp we passed */
                                }, &sentinel);
  ASSERT_NE(sock, nullptr);

  /* Trigger callback via timeout and verify userp in a different way:
   * use a struct to capture both the arg pointer and a flag */
  struct Ctx {
    void *received_arg;
    int   fired;
  } ctx = {nullptr, 0};

  /* Recreate with a ctx that captures the arg */
  xSocketDestroy(loop, sock);

  sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                        xEvent_Read,
                        [](xSocket, xEventMask, void *arg) {
                          auto *c = static_cast<Ctx *>(arg);
                          c->received_arg = arg;
                          c->fired = 1;
                        }, &ctx);
  ASSERT_NE(sock, nullptr);

  xSocketSetTimeout(sock, 50, 0);
  pump_loop(loop, 200);

  EXPECT_EQ(ctx.fired, 1);
  EXPECT_EQ(ctx.received_arg, &ctx);

  xSocketDestroy(loop, sock);
  xEventLoopDestroy(loop);
}

TEST(SocketCallback, MaskReflectsEvent) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  /* Test 1: Timeout event mask */
  xEventMask timeout_mask = 0;

  xSocket sock = xSocketCreate(loop, AF_INET, SOCK_STREAM, 0,
                                xEvent_Read,
                                [](xSocket, xEventMask m, void *arg) {
                                  *static_cast<xEventMask *>(arg) = m;
                                }, &timeout_mask);
  ASSERT_NE(sock, nullptr);

  xSocketSetTimeout(sock, 50, 0);
  pump_loop(loop, 200);

  EXPECT_TRUE(timeout_mask & xEvent_Timeout);
  EXPECT_FALSE(timeout_mask & xEvent_Read);
  EXPECT_FALSE(timeout_mask & xEvent_Write);

  xSocketDestroy(loop, sock);

  /* Test 2: Write event mask — use a socketpair where write end is writable */
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

  xEventMask io_mask = 0;

  /* Register fds[0] for write — socketpair fds are always writable initially */
  xEventSource src = xEventAdd(loop, fds[0], xEvent_Write,
      [](int, xEventMask m, void *arg) {
        *static_cast<xEventMask *>(arg) = m;
      }, &io_mask);
  ASSERT_NE(src, nullptr);

  xEventWait(loop, 100);
  EXPECT_TRUE(io_mask & xEvent_Write);

  xEventDel(loop, src);

  /* Test 3: Read event mask — write data to trigger read */
  xEventMask read_mask = 0;

  xEventSource src2 = xEventAdd(loop, fds[0], xEvent_Read,
      [](int fd, xEventMask m, void *arg) {
        *static_cast<xEventMask *>(arg) = m;
        drain_fd(fd);
      }, &read_mask);
  ASSERT_NE(src2, nullptr);

  write(fds[1], "x", 1);
  xEventWait(loop, 100);
  EXPECT_TRUE(read_mask & xEvent_Read);

  xEventDel(loop, src2);
  close(fds[0]);
  close(fds[1]);
  xEventLoopDestroy(loop);
}
