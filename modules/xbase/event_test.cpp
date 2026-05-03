/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_test.cpp - Unit tests for xEventLoop
 */

#include <xbase/event.h>

#include <atomic>
#include <chrono>
#include <cstring>
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

/* Create a non-blocking socket pair (cross-platform). Returns 0 on success. */
static int make_pipe(int fds[2]) {
#ifdef _WIN32
  /* Ensure Winsock is initialized */
  static bool wsa_init = false;
  if (!wsa_init) {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    wsa_init = true;
  }
  /* Create a loopback socket pair */
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(listener);
    return -1;
  }
  if (listen(listener, 1) != 0) {
    closesocket(listener);
    return -1;
  }
  int addrlen = sizeof(addr);
  if (getsockname(listener, (struct sockaddr *)&addr, &addrlen) != 0) {
    closesocket(listener);
    return -1;
  }

  SOCKET conn = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn == INVALID_SOCKET) {
    closesocket(listener);
    return -1;
  }
  if (connect(conn, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    closesocket(listener);
    closesocket(conn);
    return -1;
  }

  SOCKET acceptor = accept(listener, NULL, NULL);
  closesocket(listener);
  if (acceptor == INVALID_SOCKET) {
    closesocket(conn);
    return -1;
  }

  /* Set non-blocking */
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

/* Drain all data from a non-blocking fd. */
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

/* ───────────────────── Create / Destroy ───────────────────── */

TEST(EventLifecycle, CreateAndDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopDestroy(loop);
}

TEST(EventLifecycle, DestroyNull) {
  xEventLoopDestroy(NULL); /* must not crash */
}

TEST(EventLifecycle, DestroyWithRegisteredSources) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read, [](int, xEventMask, void *) {}, nullptr);
  ASSERT_NE(src, nullptr);

  /* Destroy without removing source — must not crash or leak */
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Add / Del ───────────────────── */

TEST(EventAddDel, AddAndDel) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read, [](int, xEventMask, void *) {}, nullptr);
  ASSERT_NE(src, nullptr);

  EXPECT_EQ(xEventDel(loop, src), xErrno_Ok);

  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

TEST(EventAddDel, AddNullCallback) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xEventSource src = xEventAdd(loop, fds[0], xEvent_Read, NULL, nullptr);
  EXPECT_EQ(src, nullptr);

  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

TEST(EventAddDel, DelNullArgs) {
  EXPECT_EQ(xEventDel(NULL, NULL), xErrno_InvalidArg);
}

TEST(EventAddDel, AddMultipleSources) {
  const int    N = 8;
  int          pipes[N][2];
  xEventSource srcs[N];

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(make_pipe(pipes[i]), 0);
    srcs[i] = xEventAdd(
      loop, pipes[i][0], xEvent_Read, [](int, xEventMask, void *) {}, nullptr);
    ASSERT_NE(srcs[i], nullptr);
  }

  /* Remove in reverse order */
  for (int i = N - 1; i >= 0; i--) {
    EXPECT_EQ(xEventDel(loop, srcs[i]), xErrno_Ok);
    close_fd(pipes[i][0]);
    close_fd(pipes[i][1]);
  }

  xEventLoopDestroy(loop);
}

/* ───────────────────── Read events ───────────────────── */

TEST(EventRead, SingleReadEvent) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  struct Ctx {
    int        fd;
    xEventMask mask;
    int        count;
  } ctx = {-1, 0, 0};

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read,
    [](int fd, xEventMask mask, void *arg) {
      auto *c = static_cast<Ctx *>(arg);
      c->fd   = fd;
      c->mask = mask;
      c->count++;
      drain_fd(fd);
    },
    &ctx);
  ASSERT_NE(src, nullptr);

  /* Write data to trigger read event */
  write_fd(fds[1], "hello", 5);

  int n = xEventWait(loop, 100);
  EXPECT_GE(n, 1);
  EXPECT_EQ(ctx.fd, fds[0]);
  EXPECT_TRUE(ctx.mask & xEvent_Read);
  EXPECT_EQ(ctx.count, 1);

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

TEST(EventRead, MultipleReadEvents) {
  const int N = 4;
  int       pipes[N][2];
  int       counts[N] = {};

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  struct Ctx {
    int *count;
    int  rfd;
  };
  std::vector<Ctx> ctxs(N);
  xEventSource     srcs[N];

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(make_pipe(pipes[i]), 0);
    ctxs[i] = {&counts[i], pipes[i][0]};
    srcs[i] = xEventAdd(
      loop, pipes[i][0], xEvent_Read,
      [](int fd, xEventMask, void *arg) {
        auto *c = static_cast<Ctx *>(arg);
        (*c->count)++;
        drain_fd(fd);
      },
      &ctxs[i]);
    ASSERT_NE(srcs[i], nullptr);
  }

  /* Write to all pipes */
  for (int i = 0; i < N; i++)
    write_fd(pipes[i][1], "x", 1);

  int total = xEventWait(loop, 200);
  EXPECT_EQ(total, N);

  for (int i = 0; i < N; i++) {
    EXPECT_EQ(counts[i], 1);
    xEventDel(loop, srcs[i]);
    close_fd(pipes[i][0]);
    close_fd(pipes[i][1]);
  }

  xEventLoopDestroy(loop);
}

/* ───────────────────── Write events ───────────────────── */

TEST(EventWrite, WriteReady) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> fired{0};

  xEventSource src = xEventAdd(
    loop, fds[1], xEvent_Write,
    [](int, xEventMask mask, void *arg) {
      auto *f = static_cast<std::atomic<int> *>(arg);
      if (mask & xEvent_Write) f->fetch_add(1);
    },
    &fired);
  ASSERT_NE(src, nullptr);

  /* Pipe write end should be immediately writable */
  int n = xEventWait(loop, 100);
  EXPECT_GE(n, 1);
  EXPECT_GE(fired.load(), 1);

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Mod ───────────────────── */

TEST(EventMod, SwitchReadToWrite) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xEventMask last_mask = 0;

  xEventSource src = xEventAdd(
    loop, fds[1], xEvent_Read,
    [](int, xEventMask mask, void *arg) {
      *static_cast<xEventMask *>(arg) = mask;
    },
    &last_mask);
  ASSERT_NE(src, nullptr);

  /* No data to read — should timeout */
  int n = xEventWait(loop, 50);
  EXPECT_EQ(n, 0);

  /* Switch to write — pipe write end is writable */
  EXPECT_EQ(xEventMod(loop, src, xEvent_Write), xErrno_Ok);

  n = xEventWait(loop, 100);
  EXPECT_GE(n, 1);
  EXPECT_TRUE(last_mask & xEvent_Write);

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

TEST(EventMod, NullArgs) {
  EXPECT_EQ(xEventMod(NULL, NULL, xEvent_Read), xErrno_InvalidArg);
}

/* ───────────────────── Timeout ───────────────────── */

TEST(EventTimeout, ZeroTimeoutReturnsImmediately) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  auto start   = std::chrono::steady_clock::now();
  int  n       = xEventWait(loop, 0);
  auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(n, 0);
  EXPECT_LT(std::chrono::duration_cast<ms>(elapsed).count(), 50);

  xEventLoopDestroy(loop);
}

TEST(EventTimeout, TimesOutWhenNoEvents) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  auto start = std::chrono::steady_clock::now();
  int  n     = xEventWait(loop, 80);
  auto elapsed =
    std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - start)
      .count();

  EXPECT_EQ(n, 0);
  EXPECT_GE(elapsed, 50); /* should have waited ~80ms */

  xEventLoopDestroy(loop);
}

/* ───────────────────── Wake ───────────────────── */

TEST(EventWake, WakeFromAnotherThread) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::thread waker([&]() {
    sleep_ms(50);
    xEventWake(loop);
  });

  auto start = std::chrono::steady_clock::now();
  int  n     = xEventWait(loop, 5000); /* long timeout, should be woken early */
  auto elapsed =
    std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - start)
      .count();

  /* Should have returned well before the 5s timeout */
  EXPECT_LT(elapsed, 2000);
  EXPECT_EQ(n, 0); /* wake itself doesn't count as a dispatched event */

  waker.join();
  xEventLoopDestroy(loop);
}

TEST(EventWake, MultipleWakesCoalesce) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  /* Send multiple wakes before waiting */
  for (int i = 0; i < 10; i++)
    EXPECT_EQ(xEventWake(loop), xErrno_Ok);

  /* Should return immediately, not hang */
  int n = xEventWait(loop, 100);
  EXPECT_EQ(n, 0);

  xEventLoopDestroy(loop);
}

TEST(EventWake, NullArg) {
  EXPECT_EQ(xEventWake(NULL), xErrno_InvalidArg);
}

/* ───────────────────── Edge-triggered semantics ───────────────────── */

TEST(EventEdgeTriggered, NoRenotifyWithoutDrain) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count = 0;

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read,
    [](int, xEventMask, void *arg) {
      /* Intentionally do NOT drain the fd */
      (*static_cast<int *>(arg))++;
    },
    &count);
  ASSERT_NE(src, nullptr);

  /* Write once */
  write_fd(fds[1], "x", 1);

  /* First wait: should fire */
  int n = xEventWait(loop, 100);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(count, 1);

  /* Second wait: edge-triggered should NOT re-fire (no new data) */
  n = xEventWait(loop, 50);
  EXPECT_EQ(n, 0);
  EXPECT_EQ(count, 1);

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

TEST(EventEdgeTriggered, RefiresOnNewData) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count = 0;

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      (*static_cast<int *>(arg))++;
      drain_fd(fd);
    },
    &count);
  ASSERT_NE(src, nullptr);

  /* First write + wait */
  write_fd(fds[1], "a", 1);
  int n = xEventWait(loop, 100);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(count, 1);

  /* Second write + wait: should fire again */
  write_fd(fds[1], "b", 1);
  n = xEventWait(loop, 100);
  EXPECT_EQ(n, 1);
  EXPECT_EQ(count, 2);

  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Concurrent usage ───────────────────── */

TEST(EventConcurrent, WakeWhileWaiting) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  std::atomic<int> read_count{0};

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      static_cast<std::atomic<int> *>(arg)->fetch_add(1);
      drain_fd(fd);
    },
    &read_count);
  ASSERT_NE(src, nullptr);

  /* Writer thread: write data and wake */
  std::thread writer([&]() {
    sleep_ms(30);
    write_fd(fds[1], "data", 4);
    xEventWake(loop);
  });

  int n = xEventWait(loop, 3000);
  EXPECT_GE(n, 1);
  EXPECT_EQ(read_count.load(), 1);

  writer.join();
  xEventDel(loop, src);
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Stress test ───────────────────── */

TEST(EventStress, ManySourcesManyEvents) {
  const int N = 16;
  int       pipes[N][2];
  int       counts[N] = {};

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  struct Ctx {
    int *count;
  };
  std::vector<Ctx> ctxs(N);
  xEventSource     srcs[N];

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(make_pipe(pipes[i]), 0);
    ctxs[i] = {&counts[i]};
    srcs[i] = xEventAdd(
      loop, pipes[i][0], xEvent_Read,
      [](int fd, xEventMask, void *arg) {
        (*static_cast<Ctx *>(arg)->count)++;
        drain_fd(fd);
      },
      &ctxs[i]);
    ASSERT_NE(srcs[i], nullptr);
  }

  /* Multiple rounds of writes */
  const int ROUNDS = 5;
  for (int r = 0; r < ROUNDS; r++) {
    for (int i = 0; i < N; i++)
      write_fd(pipes[i][1], "x", 1);

    int total = 0;
    /* May need multiple waits to collect all events */
    for (int attempt = 0; attempt < 10 && total < N; attempt++) {
      int n = xEventWait(loop, 100);
      if (n > 0) total += n;
    }
    EXPECT_EQ(total, N) << "round " << r;
  }

  for (int i = 0; i < N; i++) {
    EXPECT_EQ(counts[i], ROUNDS);
    xEventDel(loop, srcs[i]);
    close_fd(pipes[i][0]);
    close_fd(pipes[i][1]);
  }

  xEventLoopDestroy(loop);
}

/* ───────────────────── Add/Del during wait ───────────────────── */

TEST(EventDynamic, AddSourceBetweenWaits) {
  int fds1[2], fds2[2];
  ASSERT_EQ(make_pipe(fds1), 0);
  ASSERT_EQ(make_pipe(fds2), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count1 = 0, count2 = 0;

  xEventSource src1 = xEventAdd(
    loop, fds1[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      (*static_cast<int *>(arg))++;
      drain_fd(fd);
    },
    &count1);
  ASSERT_NE(src1, nullptr);

  /* First event on src1 */
  write_fd(fds1[1], "a", 1);
  xEventWait(loop, 100);
  EXPECT_EQ(count1, 1);

  /* Add second source between waits */
  xEventSource src2 = xEventAdd(
    loop, fds2[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      (*static_cast<int *>(arg))++;
      drain_fd(fd);
    },
    &count2);
  ASSERT_NE(src2, nullptr);

  /* Trigger both */
  write_fd(fds1[1], "b", 1);
  write_fd(fds2[1], "c", 1);

  int total = 0;
  for (int i = 0; i < 5 && total < 2; i++) {
    int n = xEventWait(loop, 100);
    if (n > 0) total += n;
  }
  EXPECT_EQ(total, 2);
  EXPECT_EQ(count1, 2);
  EXPECT_EQ(count2, 1);

  xEventDel(loop, src1);
  xEventDel(loop, src2);
  xEventLoopDestroy(loop);
  close_fd(fds1[0]);
  close_fd(fds1[1]);
  close_fd(fds2[0]);
  close_fd(fds2[1]);
}

TEST(EventDynamic, DelSourceBetweenWaits) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count = 0;

  xEventSource src = xEventAdd(
    loop, fds[0], xEvent_Read,
    [](int fd, xEventMask, void *arg) {
      (*static_cast<int *>(arg))++;
      drain_fd(fd);
    },
    &count);
  ASSERT_NE(src, nullptr);

  /* Fire once */
  write_fd(fds[1], "a", 1);
  xEventWait(loop, 100);
  EXPECT_EQ(count, 1);

  /* Remove source */
  xEventDel(loop, src);

  /* Write again — should not fire */
  write_fd(fds[1], "b", 1);
  int n = xEventWait(loop, 50);
  EXPECT_EQ(n, 0);
  EXPECT_EQ(count, 1);

  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Read + Write combined ───────────────────── */

TEST(EventReadWrite, BothReadAndWrite) {
  int fds[2];
  ASSERT_EQ(make_pipe(fds), 0);

  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xEventMask got_mask = 0;
  int        count    = 0;

  /* Monitor write end for both read and write.
   * Write end of a pipe is always writable. */
  auto        *ctx_pair = new std::pair<xEventMask *, int *>(&got_mask, &count);
  xEventSource src      = xEventAdd(
    loop, fds[1], (xEventMask)(xEvent_Read | xEvent_Write),
    [](int, xEventMask mask, void *arg) {
      auto *ctx = static_cast<std::pair<xEventMask *, int *> *>(arg);
      *ctx->first |= mask;
      (*ctx->second)++;
    },
    ctx_pair);
  ASSERT_NE(src, nullptr);

  /* Should get at least a write event */
  int total = 0;
  for (int i = 0; i < 5 && total == 0; i++) {
    int n = xEventWait(loop, 100);
    if (n > 0) total += n;
  }
  EXPECT_GE(total, 1);
  EXPECT_TRUE(got_mask & xEvent_Write);

  xEventDel(loop, src);
  delete ctx_pair;
  xEventLoopDestroy(loop);
  close_fd(fds[0]);
  close_fd(fds[1]);
}

/* ───────────────────── Signal watch (POSIX only) ───────────────────── */

#ifndef _WIN32
#include <csignal>
#include <sys/types.h>

TEST(EventSignal, BasicRegisterAndTrigger) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  struct Ctx {
    int signo;
    int count;
  } ctx = {0, 0};

  EXPECT_EQ(xEventLoopSignalWatch(
              loop, SIGUSR1,
              [](int signo, void *arg) {
                auto *c  = static_cast<Ctx *>(arg);
                c->signo = signo;
                c->count++;
              },
              &ctx),
            xErrno_Ok);

  kill(getpid(), SIGUSR1);

  for (int i = 0; i < 10 && ctx.count == 0; i++)
    xEventWait(loop, 100);

  EXPECT_GE(ctx.count, 1);
  EXPECT_EQ(ctx.signo, SIGUSR1);

  /* Cleanup */
  xEventLoopSignalWatch(loop, SIGUSR1, NULL, NULL);
  xEventLoopDestroy(loop);
}

TEST(EventSignal, CancelStopsCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count = 0;

  EXPECT_EQ(xEventLoopSignalWatch(
              loop, SIGUSR1,
              [](int, void *arg) { (*static_cast<int *>(arg))++; }, &count),
            xErrno_Ok);

  kill(getpid(), SIGUSR1);
  for (int i = 0; i < 10 && count == 0; i++)
    xEventWait(loop, 100);
  EXPECT_GE(count, 1);

  EXPECT_EQ(xEventLoopSignalWatch(loop, SIGUSR1, NULL, NULL), xErrno_Ok);

  signal(SIGUSR1, SIG_IGN);
  int saved = count;
  kill(getpid(), SIGUSR1);
  xEventWait(loop, 100);
  signal(SIGUSR1, SIG_DFL);

  EXPECT_EQ(count, saved);

  xEventLoopDestroy(loop);
}

TEST(EventSignal, ReplaceCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count1 = 0, count2 = 0;

  EXPECT_EQ(xEventLoopSignalWatch(
              loop, SIGUSR1,
              [](int, void *arg) { (*static_cast<int *>(arg))++; }, &count1),
            xErrno_Ok);

  EXPECT_EQ(xEventLoopSignalWatch(
              loop, SIGUSR1,
              [](int, void *arg) { (*static_cast<int *>(arg))++; }, &count2),
            xErrno_Ok);

  kill(getpid(), SIGUSR1);

  for (int i = 0; i < 10 && count2 == 0; i++)
    xEventWait(loop, 100);

  EXPECT_EQ(count1, 0);
  EXPECT_GE(count2, 1);

  xEventLoopSignalWatch(loop, SIGUSR1, NULL, NULL);
  xEventLoopDestroy(loop);
}

TEST(EventSignal, InvalidArgs) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  auto dummy = [](int, void *) {};

  EXPECT_EQ(xEventLoopSignalWatch(NULL, SIGUSR1, dummy, NULL),
            xErrno_InvalidArg);
  EXPECT_EQ(xEventLoopSignalWatch(loop, SIGKILL, dummy, NULL),
            xErrno_InvalidArg);
  EXPECT_EQ(xEventLoopSignalWatch(loop, SIGSTOP, dummy, NULL),
            xErrno_InvalidArg);
  EXPECT_EQ(xEventLoopSignalWatch(loop, -1, dummy, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xEventLoopSignalWatch(loop, 0, dummy, NULL), xErrno_InvalidArg);

  xEventLoopDestroy(loop);
}

TEST(EventSignal, MultipleSignals) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  int count1 = 0, count2 = 0;

  EXPECT_EQ(xEventLoopSignalWatch(
              loop, SIGUSR1,
              [](int, void *arg) { (*static_cast<int *>(arg))++; }, &count1),
            xErrno_Ok);

  EXPECT_EQ(xEventLoopSignalWatch(
              loop, SIGUSR2,
              [](int, void *arg) { (*static_cast<int *>(arg))++; }, &count2),
            xErrno_Ok);

  kill(getpid(), SIGUSR1);
  kill(getpid(), SIGUSR2);

  for (int i = 0; i < 10 && (count1 == 0 || count2 == 0); i++)
    xEventWait(loop, 100);

  EXPECT_GE(count1, 1);
  EXPECT_GE(count2, 1);

  xEventLoopSignalWatch(loop, SIGUSR1, NULL, NULL);
  xEventLoopSignalWatch(loop, SIGUSR2, NULL, NULL);
  xEventLoopDestroy(loop);
}

TEST(EventSignal, StopLoopFromCallback) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  EXPECT_EQ(
    xEventLoopSignalWatch(
      loop, SIGUSR1,
      [](int, void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); },
      loop),
    xErrno_Ok);

  std::thread sender([&]() {
    sleep_ms(50);
    kill(getpid(), SIGUSR1);
  });

  auto start = std::chrono::steady_clock::now();
  xEventLoopRun(loop);
  auto elapsed =
    std::chrono::duration_cast<ms>(std::chrono::steady_clock::now() - start)
      .count();

  EXPECT_LT(elapsed, 3000);

  sender.join();
  xEventLoopSignalWatch(loop, SIGUSR1, NULL, NULL);
  xEventLoopDestroy(loop);
}
#endif /* _WIN32 */
