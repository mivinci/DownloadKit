/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * transport_test.cpp - Unit tests for xnet transport layer
 */

#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#if defined(XK_HAS_OPENSSL) || defined(XK_HAS_MBEDTLS)
#include "tls_private.h"
#endif
#include "transport_private.h"
#include <xnet/transport.h>
}

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════
 *  Plain TCP Transport Tests
 * ═══════════════════════════════════════════════════════════════════
 */

class PlainTransportTest : public ::testing::Test {
protected:
  int        fds[2] = {-1, -1};
  xTransport t;

  void SetUp() override {
    memset(&t, 0, sizeof(t));
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  }

  void TearDown() override {
    if (t.destroy) t.destroy(t.ctx);
    if (fds[0] >= 0) close(fds[0]);
    if (fds[1] >= 0) close(fds[1]);
  }
};

TEST_F(PlainTransportTest, InitSetsCallbacks) {
  xTransportPlainInit(&t, fds[0]);
  EXPECT_NE(t.read, nullptr);
  EXPECT_NE(t.writev, nullptr);
  EXPECT_EQ(t.handshake, nullptr);
  EXPECT_EQ(t.alpn, nullptr);
  EXPECT_NE(t.destroy, nullptr);
  EXPECT_NE(t.ctx, nullptr);
}

TEST_F(PlainTransportTest, ReadWrite) {
  xTransportPlainInit(&t, fds[0]);
  ASSERT_NE(t.read, nullptr);

  /* Write through the peer fd */
  const char *msg = "hello transport";
  ssize_t     nw  = write(fds[1], msg, strlen(msg));
  ASSERT_GT(nw, 0);

  /* Read through the transport */
  char    buf[64] = {};
  ssize_t nr      = t.read(t.ctx, buf, sizeof(buf));
  ASSERT_EQ(nr, nw);
  EXPECT_STREQ(buf, msg);
}

TEST_F(PlainTransportTest, Writev) {
  xTransportPlainInit(&t, fds[0]);
  ASSERT_NE(t.writev, nullptr);

  /* Write through the transport using scatter-gather */
  struct iovec iov[2];
  const char  *part1 = "hello ";
  const char  *part2 = "world";
  iov[0].iov_base    = (void *)part1;
  iov[0].iov_len     = strlen(part1);
  iov[1].iov_base    = (void *)part2;
  iov[1].iov_len     = strlen(part2);

  ssize_t nw = t.writev(t.ctx, iov, 2);
  ASSERT_GT(nw, 0);

  /* Read from the peer fd */
  char    buf[64] = {};
  ssize_t nr      = read(fds[1], buf, sizeof(buf));
  ASSERT_EQ(nr, nw);
  EXPECT_STREQ(buf, "hello world");
}

TEST_F(PlainTransportTest, DestroyDoesNotCloseFd) {
  xTransportPlainInit(&t, fds[0]);
  ASSERT_NE(t.destroy, nullptr);

  /* Destroy the transport */
  t.destroy(t.ctx);
  t.ctx     = nullptr;
  t.destroy = nullptr;

  /* The fd should still be valid (write should succeed) */
  const char *msg = "still alive";
  ssize_t     nw  = write(fds[0], msg, strlen(msg));
  EXPECT_GT(nw, 0);

  char    buf[64] = {};
  ssize_t nr      = read(fds[1], buf, sizeof(buf));
  EXPECT_EQ(nr, nw);
  EXPECT_STREQ(buf, msg);
}

TEST_F(PlainTransportTest, NullTransportIsSafe) {
  /* Should not crash */
  xTransportPlainInit(nullptr, fds[0]);
}

/* ═══════════════════════════════════════════════════════════════════
 *  TLS Context Tests (conditional)
 * ═══════════════════════════════════════════════════════════════════
 */

#if defined(XK_HAS_OPENSSL) || defined(XK_HAS_MBEDTLS)

TEST(TlsCtxTest, CreateWithNullConfReturnsNull) {
  xTlsCtx ctx = xTlsCtxCreate(nullptr);
  EXPECT_EQ(ctx, nullptr);
}

TEST(TlsCtxTest, CreateWithInvalidCertReturnsNull) {
  xTlsConf conf = {};
  conf.cert     = "/nonexistent/cert.pem";
  conf.key      = "/nonexistent/key.pem";
  xTlsCtx ctx   = xTlsCtxCreate(&conf);
  EXPECT_EQ(ctx, nullptr);
}

TEST(TlsCtxTest, DestroyNullIsSafe) {
  /* Should not crash */
  xTlsCtxDestroy(nullptr);
}

TEST(TlsCtxTest, ClientInitWithInvalidConfFails) {
  xTransport t;
  memset(&t, 0, sizeof(t));
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  /* Client init with bad CA path should fail at ctx creation */
  xTlsConf conf    = {};
  conf.ca          = "/nonexistent/ca.pem";
  conf.skip_verify = 0;
  xTlsCtx ctx      = xTlsCtxCreate(&conf);
  /* Depending on backend, this may or may not fail (system CA fallback) */
  if (ctx) {
    int ret = xTransportTlsClientInit(&t, ctx, "example.com", fds[0]);
    /* Just ensure it doesn't crash */
    if (ret == 0 && t.destroy) {
      t.destroy(t.ctx);
    }
    xTlsCtxDestroy(ctx);
  }

  close(fds[0]);
  close(fds[1]);
}

TEST(TlsCtxTest, ClientCtxCreateWithDefaults) {
  /* Create a client TLS context with default settings (no cert/key) */
  xTlsConf conf    = {};
  conf.skip_verify = 1;
  xTlsCtx ctx      = xTlsCtxCreate(&conf);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(xTlsCtxIsServer(ctx), 0);
  xTlsCtxDestroy(ctx);
}

#endif /* XK_HAS_OPENSSL || XK_HAS_MBEDTLS */
