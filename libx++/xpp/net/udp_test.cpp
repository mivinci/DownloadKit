/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * udp_test.cpp - Unit tests for xpp::net::UdpSocket.
 */

#include <xpp/net/udp.h>
#include <xpp/runtime.h>
#include <gtest/gtest.h>

#include <cstring>
#include <sys/socket.h>

using namespace xpp;
using namespace xpp::net;

/* ── Test fixture ──────────────────────────────────────────────────── */

class UdpSocketTest : public ::testing::Test {
protected:
  void SetUp() override { m_rt = new Runtime(2); }
  void TearDown() override { delete m_rt; }
  Runtime *m_rt;
};

/* ── bind + local_addr ─────────────────────────────────────────────── */

TEST_F(UdpSocketTest, BindLoopback) {
  auto guard = m_rt->enter();
  auto r = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0)));
  ASSERT_TRUE(r.is_ok());
  auto sock = std::move(r.unwrap());
  EXPECT_FALSE(sock.is_closed());
  auto addr = sock.local_addr();
  EXPECT_TRUE(addr.is_ipv4());
  EXPECT_EQ(addr.as_v4().ip(), Ipv4Addr::LOCALHOST);
  EXPECT_NE(addr.port(), 0);
  sock.close();
}

TEST_F(UdpSocketTest, BindV6Any) {
  auto guard = m_rt->enter();
  auto r = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV6::from(Ipv6Addr::UNSPECIFIED, 0, 0, 0)));
  ASSERT_TRUE(r.is_ok());
  auto sock = std::move(r.unwrap());
  auto addr = sock.local_addr();
  EXPECT_TRUE(addr.is_ipv6());
  sock.close();
}

TEST_F(UdpSocketTest, Close) {
  auto guard = m_rt->enter();
  auto r = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0)));
  ASSERT_TRUE(r.is_ok());
  auto sock = std::move(r.unwrap());
  EXPECT_FALSE(sock.is_closed());
  sock.close();
  EXPECT_TRUE(sock.is_closed());
  sock.close(); // idempotent
  EXPECT_TRUE(sock.is_closed());
}

/* ── send_to + recv_from ───────────────────────────────────────────── */

TEST_F(UdpSocketTest, SendToRecvFrom) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  const char *msg = "hello";
  char        recv_buf[64];

  auto sent = a.send_to(Span<const char>(msg, 5), b.local_addr()).wait(guard);
  EXPECT_EQ(sent, 5);

  auto result = b.recv_from(Span<char>(recv_buf, sizeof(recv_buf))).wait(guard);
  EXPECT_EQ(result.n, 5);
  EXPECT_EQ(memcmp(recv_buf, "hello", 5), 0);
  EXPECT_EQ(result.addr.port(), a.local_addr().port());
}

TEST_F(UdpSocketTest, SendToRecvFromLargeDatagram) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  char send_buf[1024];
  memset(send_buf, 'A', sizeof(send_buf));

  a.send_to(Span<const char>(send_buf, sizeof(send_buf)), b.local_addr()).wait(guard);

  char recv_buf[2048];
  auto result = b.recv_from(Span<char>(recv_buf, sizeof(recv_buf))).wait(guard);
  EXPECT_EQ(result.n, 1024);
  EXPECT_EQ(memcmp(recv_buf, send_buf, 1024), 0);
}

/* ── connect + send / recv ─────────────────────────────────────────── */

TEST_F(UdpSocketTest, ConnectSendRecv) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  EXPECT_TRUE(a.connect_addr(b.local_addr()).is_ok());

  const char *msg = "ping";
  char        recv_buf[64];

  a.send(Span<const char>(msg, 4)).wait(guard);

  auto result = b.recv_from(Span<char>(recv_buf, sizeof(recv_buf))).wait(guard);
  EXPECT_EQ(result.n, 4);
  EXPECT_EQ(memcmp(recv_buf, "ping", 4), 0);
}

/* ── echo round-trip ───────────────────────────────────────────────── */

TEST_F(UdpSocketTest, EchoRoundTrip) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  const char *msg = "echo-test";
  size_t      msg_len = strlen(msg);
  char        recv_buf[64];

  // A → B
  a.send_to(Span<const char>(msg, msg_len), b.local_addr()).wait(guard);
  auto r1 = b.recv_from(Span<char>(recv_buf, sizeof(recv_buf))).wait(guard);
  EXPECT_EQ(r1.n, static_cast<ssize_t>(msg_len));
  EXPECT_EQ(memcmp(recv_buf, msg, msg_len), 0);

  // B → A (echo)
  b.send_to(Span<const char>(recv_buf, r1.n), r1.addr).wait(guard);
  auto r2 = a.recv_from(Span<char>(recv_buf, sizeof(recv_buf))).wait(guard);
  EXPECT_EQ(r2.n, static_cast<ssize_t>(msg_len));
  EXPECT_EQ(memcmp(recv_buf, msg, msg_len), 0);
  EXPECT_EQ(r2.addr.port(), b.local_addr().port());
}

/* ── multiple datagrams ────────────────────────────────────────────── */

TEST_F(UdpSocketTest, MultipleDatagrams) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  for (int i = 0; i < 5; ++i) {
    char msg[16];
    int  len = snprintf(msg, sizeof(msg), "msg-%d", i);
    a.send_to(Span<const char>(msg, len), b.local_addr()).wait(guard);
  }

  for (int i = 0; i < 5; ++i) {
    char recv_buf[64];
    auto result = b.recv_from(Span<char>(recv_buf, sizeof(recv_buf))).wait(guard);
    char expected[16];
    int  expected_len = snprintf(expected, sizeof(expected), "msg-%d", i);
    EXPECT_EQ(result.n, expected_len);
    EXPECT_EQ(memcmp(recv_buf, expected, expected_len), 0);
  }
}

/* ── bind("host:port") hostname overload ─────────────────────────── */

TEST_F(UdpSocketTest, BindHostPortLoopback) {
  auto r = m_rt->block_on([&] { return UdpSocket::bind("127.0.0.1:0"); });
  ASSERT_TRUE(r.is_ok());
  auto sock = std::move(r).unwrap();
  EXPECT_FALSE(sock.is_closed());
  EXPECT_NE(sock.local_addr().port(), 0);
}

TEST_F(UdpSocketTest, BindHostPortLocalhost) {
  auto r = m_rt->block_on([&] { return UdpSocket::bind("localhost:0"); });
  ASSERT_TRUE(r.is_ok());
}

TEST_F(UdpSocketTest, BindHostPortBogus) {
  auto r = m_rt->block_on([&] { return UdpSocket::bind("nonexistent.invalid:0"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err(), SocketError::ResolveFailed);
}

TEST_F(UdpSocketTest, ConnectHostPort) {
  // Bind an echo target on an ephemeral port (synchronous bind_addr is
  // OK without a runtime context).
  auto target = UdpSocket::bind_addr(
                  SocketAddr::from(SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0)))
                  .unwrap();
  auto port = target.local_addr().port();

  // Bind a sender and connect by hostname:port.
  auto sender_r = m_rt->block_on([&] { return UdpSocket::bind("127.0.0.1:0"); });
  ASSERT_TRUE(sender_r.is_ok());
  auto sender = std::move(sender_r).unwrap();

  char addr[32];
  std::snprintf(addr, sizeof(addr), "127.0.0.1:%u", static_cast<unsigned>(port));
  auto cr = m_rt->block_on(
    [&]() -> Promise<Result<void, SocketError>> { return sender.connect(addr); });
  EXPECT_TRUE(cr.is_ok());
}
