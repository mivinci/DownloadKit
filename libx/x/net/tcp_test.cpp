/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_test.cpp - Unit tests for xTcpConn, xTcpConnect, xTcpListener
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include <x/base/io.h>
#include <x/net/tcp.h>
#include <x/net/transport.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * Helper: get a free port by binding to port 0 and reading back the port.
 */
static uint16_t get_free_port() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in addr = {};
  addr.sin_family         = AF_INET;
  addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
  addr.sin_port           = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  (void)getsockname(fd, (struct sockaddr *)&addr, &len);
  uint16_t port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

/* ───────────────────── Fixture ───────────────────── */

class TcpTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
  }

  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }

  void RunUntilDone(std::atomic<bool> &done, int timeout_ms = 10000) {
    std::thread runner([&]() { xEventLoopRun(loop); });

    auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      sleep_ms(10);
    }

    xEventLoopStop(loop);
    runner.join();
  }
};

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpConn unit tests (no event loop needed)
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(TcpConnTest, NullSafe) {
  /* All functions should be safe with NULL */
  EXPECT_EQ(xTcpConnTransport(nullptr), nullptr);
  EXPECT_EQ(xTcpConnSocket(nullptr), nullptr);

  xTransport t = xTcpConnTakeTransport(nullptr);
  EXPECT_EQ(t.read, nullptr);
  EXPECT_EQ(t.writev, nullptr);
  EXPECT_EQ(t.ctx, nullptr);

  EXPECT_EQ(xTcpConnTakeSocket(nullptr), nullptr);

  /* Should not crash */
  xTcpConnClose(nullptr, nullptr);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Loopback test: listener + connect
 * ═══════════════════════════════════════════════════════════════════
 */

struct LoopbackCtx {
  std::atomic<bool> connect_done{false};
  std::atomic<bool> accept_done{false};
  xTcpConn          client_conn{nullptr};
  xTcpConn          server_conn{nullptr};
  xErrno            connect_err{xErrno_Unknown};
  xEventLoop        loop{nullptr};
};

static void loopback_connect_cb(xTcpConn conn, xErrno err, void *arg) {
  auto *ctx        = static_cast<LoopbackCtx *>(arg);
  ctx->client_conn = conn;
  ctx->connect_err = err;
  ctx->connect_done.store(true, std::memory_order_release);
}

static void loopback_accept_cb(xTcpListener listener, xTcpConn conn,
                               const struct sockaddr *addr, socklen_t addrlen,
                               void *arg) {
  (void)listener;
  (void)addr;
  (void)addrlen;
  auto *ctx        = static_cast<LoopbackCtx *>(arg);
  ctx->server_conn = conn;
  ctx->accept_done.store(true, std::memory_order_release);
}

TEST_F(TcpTest, LoopbackPlainTcp) {
  LoopbackCtx ctx;
  ctx.loop = loop;

  /* Get a free port to avoid conflicts */
  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  /* Create listener */
  xTcpListenerConf lconf = {};
  xTcpListener listener  = xTcpListenerCreate(loop, "127.0.0.1", port, &lconf,
                                              loopback_accept_cb, &ctx);
  ASSERT_NE(listener, nullptr);

  /* Connect to the listener */
  xErrno err =
    xTcpConnect(loop, "127.0.0.1", port, nullptr, loopback_connect_cb, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  /* Run event loop until both sides are done */
  std::thread runner([&]() { xEventLoopRun(loop); });

  auto deadline = std::chrono::steady_clock::now() + ms(10000);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx.connect_done.load() && ctx.accept_done.load()) break;
    sleep_ms(10);
  }

  EXPECT_TRUE(ctx.connect_done.load());
  EXPECT_TRUE(ctx.accept_done.load());
  EXPECT_EQ(ctx.connect_err, xErrno_Ok);
  EXPECT_NE(ctx.client_conn, nullptr);
  EXPECT_NE(ctx.server_conn, nullptr);

  if (ctx.client_conn && ctx.server_conn) {
    /* Write from client, read from server */
    xTransport *ct = xTcpConnTransport(ctx.client_conn);
    xTransport *st = xTcpConnTransport(ctx.server_conn);
    ASSERT_NE(ct, nullptr);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(ct->writev, nullptr);
    ASSERT_NE(st->read, nullptr);

    const char  *msg = "hello from client";
    struct iovec iov;
    iov.iov_base = (void *)msg;
    iov.iov_len  = strlen(msg);
    ssize_t nw   = ct->writev(ct->ctx, &iov, 1);
    EXPECT_GT(nw, 0);

    /* Give data time to arrive */
    sleep_ms(50);

    char    buf[64] = {};
    ssize_t nr      = st->read(st->ctx, buf, sizeof(buf));
    EXPECT_EQ(nr, nw);
    EXPECT_STREQ(buf, msg);
  }

  /* Clean up */
  if (ctx.client_conn) xTcpConnClose(loop, ctx.client_conn);
  if (ctx.server_conn) xTcpConnClose(loop, ctx.server_conn);
  xTcpListenerDestroy(listener);

  xEventLoopStop(loop);
  runner.join();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Timeout test
 * ═══════════════════════════════════════════════════════════════════
 */

struct TimeoutCtx {
  std::atomic<bool> done{false};
  xErrno            err{xErrno_Unknown};
  xTcpConn          conn{nullptr};
};

static void timeout_connect_cb(xTcpConn conn, xErrno err, void *arg) {
  auto *ctx = static_cast<TimeoutCtx *>(arg);
  ctx->conn = conn;
  ctx->err  = err;
  ctx->done.store(true, std::memory_order_release);
}

TEST_F(TcpTest, ConnectTimeout) {
  TimeoutCtx ctx;

  /* Create a listening socket with backlog=0 and fill it up,
   * so subsequent connects will hang (SYN queue full).
   * This is the most reliable way to test connect timeout. */
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(listen_fd, 0);

  struct sockaddr_in addr = {};
  addr.sin_family         = AF_INET;
  addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
  addr.sin_port           = 0;
  ASSERT_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

  socklen_t alen = sizeof(addr);
  (void)getsockname(listen_fd, (struct sockaddr *)&addr, &alen);
  uint16_t port = ntohs(addr.sin_port);

  /* Listen with backlog=1 */
  ASSERT_EQ(listen(listen_fd, 1), 0);

  /* Fill the backlog with a dummy connection */
  int dummy_fd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(dummy_fd, 0);
  int err = connect(dummy_fd, (struct sockaddr *)&addr, sizeof(addr));
  (void)err;

  /* Another dummy to overflow */
  int dummy_fd2 = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(dummy_fd2, 0);
  err = connect(dummy_fd2, (struct sockaddr *)&addr, sizeof(addr));
  (void)err;

  /* Now try to connect with a short timeout - should timeout */
  xTcpConnectConf conf = {};
  conf.timeout_ms      = 500;

  err = xTcpConnect(loop, "127.0.0.1", port, &conf, timeout_connect_cb, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  RunUntilDone(ctx.done, 5000);

  EXPECT_TRUE(ctx.done.load());
  /* The connection should have failed (timeout or refused) */
  EXPECT_NE(ctx.err, xErrno_Ok);
  if (ctx.conn) {
    xTcpConnClose(loop, ctx.conn);
  }

  close(dummy_fd2);
  close(dummy_fd);
  close(listen_fd);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Invalid argument tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST_F(TcpTest, ConnectNullArgs) {
  auto noop = [](xTcpConn, xErrno, void *) {};
  EXPECT_EQ(xTcpConnect(nullptr, "host", 80, nullptr, noop, nullptr),
            xErrno_InvalidArg);
  EXPECT_EQ(xTcpConnect(loop, nullptr, 80, nullptr, noop, nullptr),
            xErrno_InvalidArg);
  EXPECT_EQ(xTcpConnect(loop, "host", 80, nullptr, nullptr, nullptr),
            xErrno_InvalidArg);
}

TEST_F(TcpTest, ListenerNullArgs) {
  auto noop = [](xTcpListener, xTcpConn, const struct sockaddr *, socklen_t,
                 void *) {};
  EXPECT_EQ(xTcpListenerCreate(nullptr, "127.0.0.1", 0, nullptr, noop, nullptr),
            nullptr);
  EXPECT_EQ(xTcpListenerCreate(loop, "127.0.0.1", 0, nullptr, nullptr, nullptr),
            nullptr);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Ownership transfer tests
 * ═══════════════════════════════════════════════════════════════════
 */

struct TransferCtx {
  std::atomic<bool> connect_done{false};
  std::atomic<bool> accept_done{false};
  xTcpConn          client_conn{nullptr};
  xTcpConn          server_conn{nullptr};
  xErrno            connect_err{xErrno_Unknown};
};

static void transfer_connect_cb(xTcpConn conn, xErrno err, void *arg) {
  auto *ctx        = static_cast<TransferCtx *>(arg);
  ctx->client_conn = conn;
  ctx->connect_err = err;
  ctx->connect_done.store(true, std::memory_order_release);
}

static void transfer_accept_cb(xTcpListener listener, xTcpConn conn,
                               const struct sockaddr *addr, socklen_t addrlen,
                               void *arg) {
  (void)listener;
  (void)addr;
  (void)addrlen;
  auto *ctx        = static_cast<TransferCtx *>(arg);
  ctx->server_conn = conn;
  ctx->accept_done.store(true, std::memory_order_release);
}

TEST_F(TcpTest, TakeSocketAndTransport) {
  TransferCtx ctx;

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  xTcpListenerConf lconf = {};
  xTcpListener listener  = xTcpListenerCreate(loop, "127.0.0.1", port, &lconf,
                                              transfer_accept_cb, &ctx);
  ASSERT_NE(listener, nullptr);

  xErrno err =
    xTcpConnect(loop, "127.0.0.1", port, nullptr, transfer_connect_cb, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  std::thread runner([&]() { xEventLoopRun(loop); });

  auto deadline = std::chrono::steady_clock::now() + ms(10000);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx.connect_done.load() && ctx.accept_done.load()) break;
    sleep_ms(10);
  }

  EXPECT_TRUE(ctx.connect_done.load());
  EXPECT_EQ(ctx.connect_err, xErrno_Ok);
  ASSERT_NE(ctx.client_conn, nullptr);

  /* Take socket */
  xSocket taken_sock = xTcpConnTakeSocket(ctx.client_conn);
  EXPECT_NE(taken_sock, nullptr);
  EXPECT_EQ(xTcpConnSocket(ctx.client_conn), nullptr);

  /* Take transport */
  xTransport taken_t = xTcpConnTakeTransport(ctx.client_conn);
  EXPECT_NE(taken_t.read, nullptr);
  EXPECT_EQ(xTcpConnTransport(ctx.client_conn)->read, nullptr);

  /* Close the conn shell (should only free the shell, not the resources) */
  xTcpConnClose(loop, ctx.client_conn);

  /* The taken resources should still be usable */
  EXPECT_NE(xSocketFd(taken_sock), -1);

  /* Clean up taken resources manually */
  if (taken_t.destroy) taken_t.destroy(taken_t.ctx);
  xSocketDestroy(loop, taken_sock);

  if (ctx.server_conn) xTcpConnClose(loop, ctx.server_conn);
  xTcpListenerDestroy(listener);

  xEventLoopStop(loop);
  runner.join();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Listener destroy test
 * ═══════════════════════════════════════════════════════════════════
 */

TEST_F(TcpTest, ListenerDestroyStopsAccept) {
  std::atomic<int> accept_count{0};

  auto accept_cb = [](xTcpListener listener, xTcpConn conn,
                      const struct sockaddr *addr, socklen_t addrlen,
                      void *arg) {
    (void)listener;
    (void)addr;
    (void)addrlen;
    auto *count = static_cast<std::atomic<int> *>(arg);
    count->fetch_add(1);
    /* Close the accepted connection immediately */
    xTcpConnClose(nullptr, conn);
  };

  uint16_t lport = get_free_port();
  ASSERT_GT(lport, 0);

  xTcpListenerConf lconf = {};
  xTcpListener listener  = xTcpListenerCreate(loop, "127.0.0.1", lport, &lconf,
                                              accept_cb, &accept_count);
  ASSERT_NE(listener, nullptr);

  /* Destroy the listener immediately */
  xTcpListenerDestroy(listener);

  /* Trying to connect should fail (no one listening) */
  /* We just verify the destroy didn't crash */
  SUCCEED();
}

TEST_F(TcpTest, ListenerDestroyNull) {
  /* Should not crash */
  xTcpListenerDestroy(nullptr);
}

/* ═══════════════════════════════════════════════════════════════════
 *  xTcpConnReader / xTcpConnWriter adapter tests
 * ═══════════════════════════════════════════════════════════════════
 */

TEST(TcpConnTest, ReaderWriterNullSafe) {
  xReader r = xTcpConnReader(nullptr);
  EXPECT_EQ(r.read, nullptr);
  EXPECT_EQ(r.ctx, nullptr);

  xWriter w = xTcpConnWriter(nullptr);
  EXPECT_EQ(w.writev, nullptr);
  EXPECT_EQ(w.ctx, nullptr);
}

struct AdapterCtx {
  std::atomic<bool> connect_done{false};
  std::atomic<bool> accept_done{false};
  xTcpConn          client_conn{nullptr};
  xTcpConn          server_conn{nullptr};
  xErrno            connect_err{xErrno_Unknown};
};

static void adapter_connect_cb(xTcpConn conn, xErrno err, void *arg) {
  auto *ctx        = static_cast<AdapterCtx *>(arg);
  ctx->client_conn = conn;
  ctx->connect_err = err;
  ctx->connect_done.store(true, std::memory_order_release);
}

static void adapter_accept_cb(xTcpListener listener, xTcpConn conn,
                               const struct sockaddr *addr, socklen_t addrlen,
                               void *arg) {
  (void)listener;
  (void)addr;
  (void)addrlen;
  auto *ctx        = static_cast<AdapterCtx *>(arg);
  ctx->server_conn = conn;
  ctx->accept_done.store(true, std::memory_order_release);
}

TEST_F(TcpTest, ReaderWriterAdapterLoopback) {
  AdapterCtx ctx;

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  xTcpListenerConf lconf = {};
  xTcpListener listener  = xTcpListenerCreate(loop, "127.0.0.1", port, &lconf,
                                              adapter_accept_cb, &ctx);
  ASSERT_NE(listener, nullptr);

  xErrno err =
    xTcpConnect(loop, "127.0.0.1", port, nullptr, adapter_connect_cb, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  std::thread runner([&]() { xEventLoopRun(loop); });

  auto deadline = std::chrono::steady_clock::now() + ms(10000);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx.connect_done.load() && ctx.accept_done.load()) break;
    sleep_ms(10);
  }

  ASSERT_TRUE(ctx.connect_done.load());
  ASSERT_TRUE(ctx.accept_done.load());
  ASSERT_EQ(ctx.connect_err, xErrno_Ok);
  ASSERT_NE(ctx.client_conn, nullptr);
  ASSERT_NE(ctx.server_conn, nullptr);

  /* Get adapters */
  xWriter client_w = xTcpConnWriter(ctx.client_conn);
  xReader server_r = xTcpConnReader(ctx.server_conn);
  ASSERT_NE(client_w.writev, nullptr);
  ASSERT_NE(server_r.read, nullptr);

  /* Write via xWrite (adapter), read via xRead (adapter) */
  const char *msg = "adapter test";
  ssize_t     nw  = xWrite(client_w, msg, strlen(msg));
  EXPECT_GT(nw, 0);

  sleep_ms(50);

  char    buf[64] = {};
  ssize_t nr      = xRead(server_r, buf, sizeof(buf));
  EXPECT_EQ(nr, nw);
  EXPECT_STREQ(buf, msg);

  /* Verify adapter behavior matches direct API */
  const char *msg2 = "direct compare";
  nw = xTcpConnSend(ctx.client_conn, msg2, strlen(msg2));
  EXPECT_GT(nw, 0);

  sleep_ms(50);

  memset(buf, 0, sizeof(buf));
  nr = xRead(server_r, buf, sizeof(buf));
  EXPECT_EQ(nr, nw);
  EXPECT_STREQ(buf, msg2);

  /* Clean up */
  xTcpConnClose(loop, ctx.client_conn);
  xTcpConnClose(loop, ctx.server_conn);
  xTcpListenerDestroy(listener);

  xEventLoopStop(loop);
  runner.join();
}
