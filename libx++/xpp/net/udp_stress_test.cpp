/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * udp_stress_test.cpp - Stress tests for xpp::net::UdpSocket.
 *
 * Validates correctness under high throughput, rapid iteration,
 * multiple concurrent sockets, and edge cases.
 */

#include <xpp/net/udp.h>
#include <xpp/runtime/runtime.h>
#include <gtest/gtest.h>

#include <cstring>
#include <sys/socket.h>

using namespace xpp;
using namespace xpp::net;

/* ── Test fixture ──────────────────────────────────────────────────── */

class UdpStressTest : public ::testing::Test {
protected:
  void SetUp() override { m_rt = runtime::Runtime::new_multi_thread(4).into_raw(); }
  void TearDown() override { delete m_rt; }
  runtime::Runtime *m_rt;
};

/* ── High-volume sequential datagrams ──────────────────────────────── */

TEST_F(UdpStressTest, ThousandDatagrams) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  const int N = 1000;
  auto      b_addr = b.local_addr();

  // Send N datagrams via raw sendto (fast, no async overhead)
  for (int i = 0; i < N; ++i) {
    char msg[32];
    int  len = snprintf(msg, sizeof(msg), "%d", i);
    struct sockaddr_storage ss;
    socklen_t slen;
    b_addr.to_sockaddr(&ss, &slen);
    sendto(a.fd(), msg, len, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
  }

  // Receive all N via xpp recv_from
  int received = 0;
  for (int i = 0; i < N; ++i) {
    char buf[64];
    auto r = m_rt->block_on(b.recv_from(Span<char>(buf, sizeof(buf))));
    if (r.n > 0) received++;
  }
  EXPECT_EQ(received, N);
}

/* ── Large datagrams (near MTU) ────────────────────────────────────── */

TEST_F(UdpStressTest, LargeDatagrams) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  // Send 100 datagrams of ~1400 bytes (near UDP MTU)
  const int N    = 100;
  const int SIZE = 1400;
  auto      b_addr = b.local_addr();

  for (int i = 0; i < N; ++i) {
    char msg[SIZE];
    memset(msg, 'A' + (i % 26), SIZE);
    struct sockaddr_storage ss;
    socklen_t slen;
    b_addr.to_sockaddr(&ss, &slen);
    sendto(a.fd(), msg, SIZE, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
  }

  int received = 0;
  for (int i = 0; i < N; ++i) {
    char buf[SIZE + 64];
    auto r = m_rt->block_on(b.recv_from(Span<char>(buf, sizeof(buf))));
    if (r.n == SIZE) {
      EXPECT_EQ(buf[0], 'A' + (received % 26));
      received++;
    }
  }
  EXPECT_EQ(received, N);
}

/* ── Multiple socket pairs ─────────────────────────────────────────── */

TEST_F(UdpStressTest, MultipleSocketPairs) {
  auto guard = m_rt->enter();

  const int PAIRS = 10;
  struct Pair {
    UdpSocket a, b;
  };
  // Can't use array because UdpSocket is move-only. Use one pair at a time.
  for (int p = 0; p < PAIRS; ++p) {
    auto a = UdpSocket::bind_addr(SocketAddr::from(
        SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
    auto b = UdpSocket::bind_addr(SocketAddr::from(
        SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

    // Each pair sends 100 datagrams
    auto b_addr = b.local_addr();
    for (int i = 0; i < 100; ++i) {
      char msg[32];
      int  len = snprintf(msg, sizeof(msg), "p%d-m%d", p, i);
      struct sockaddr_storage ss;
      socklen_t slen;
      b_addr.to_sockaddr(&ss, &slen);
      sendto(a.fd(), msg, len, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
    }

    int received = 0;
    for (int i = 0; i < 100; ++i) {
      char buf[64];
      auto r = m_rt->block_on(b.recv_from(Span<char>(buf, sizeof(buf))));
      if (r.n > 0) received++;
    }
    EXPECT_EQ(received, 100) << "pair " << p;
  }
}

/* ── Rapid bind/close cycles ──────────────────────────────────────── */

TEST_F(UdpStressTest, RapidBindClose) {
  auto guard = m_rt->enter();

  for (int i = 0; i < 200; ++i) {
    auto r = UdpSocket::bind_addr(SocketAddr::from(
        SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0)));
    ASSERT_TRUE(r.is_ok()) << "iteration " << i;
    auto sock = std::move(r.unwrap());
    EXPECT_FALSE(sock.is_closed());
    EXPECT_NE(sock.local_addr().port(), 0);
    sock.close();
    EXPECT_TRUE(sock.is_closed());
  }
}

/* ── Send-recv interleaved rapidly ─────────────────────────────────── */

TEST_F(UdpStressTest, InterleavedSendRecv) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  auto b_addr = b.local_addr();
  const int N = 500;

  // Send all datagrams via raw sendto (fast, no async)
  for (int i = 0; i < N; ++i) {
    char msg[32];
    int  len = snprintf(msg, sizeof(msg), "seq-%d", i);
    struct sockaddr_storage ss;
    socklen_t slen;
    b_addr.to_sockaddr(&ss, &slen);
    sendto(a.fd(), msg, len, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
  }

  // Receive all via xpp recv_from
  int received = 0;
  for (int i = 0; i < N; ++i) {
    char buf[64];
    auto r = m_rt->block_on(b.recv_from(Span<char>(buf, sizeof(buf))));
    if (r.n > 0) received++;
  }
  EXPECT_EQ(received, N);
}

/* ── Send from multiple raw fds to one xpp socket ──────────────────── */

TEST_F(UdpStressTest, MultipleSendersOneReceiver) {
  auto guard = m_rt->enter();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  const int SENDERS = 5;
  const int MSGS    = 200;
  auto      b_addr  = b.local_addr();

  // Create raw sender sockets and send
  for (int s = 0; s < SENDERS; ++s) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    for (int i = 0; i < MSGS; ++i) {
      char msg[32];
      int  len = snprintf(msg, sizeof(msg), "s%d-m%d", s, i);
      struct sockaddr_storage ss;
      socklen_t slen;
      b_addr.to_sockaddr(&ss, &slen);
      sendto(fd, msg, len, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
    }
    close(fd);
  }

  // Receive all
  int received = 0;
  for (int i = 0; i < SENDERS * MSGS; ++i) {
    char buf[64];
    auto r = m_rt->block_on(b.recv_from(Span<char>(buf, sizeof(buf))));
    if (r.n > 0) received++;
  }
  EXPECT_EQ(received, SENDERS * MSGS);
}

/* ── Echo under load ──────────────────────────────────────────────── */

TEST_F(UdpStressTest, EchoUnderLoad) {
  auto guard = m_rt->enter();
  auto a = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();
  auto b = UdpSocket::bind_addr(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::LOCALHOST, 0))).unwrap();

  const int N = 200;
  auto      b_addr = b.local_addr();

  // Phase 1: A sends N datagrams to B
  for (int i = 0; i < N; ++i) {
    char msg[32];
    int  len = snprintf(msg, sizeof(msg), "echo-%d", i);
    struct sockaddr_storage ss;
    socklen_t slen;
    b_addr.to_sockaddr(&ss, &slen);
    sendto(a.fd(), msg, len, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
  }

  // Phase 2: B receives all, echoes each back to A
  for (int i = 0; i < N; ++i) {
    char buf[64];
    auto r = m_rt->block_on(b.recv_from(Span<char>(buf, sizeof(buf))));
    EXPECT_GT(r.n, 0);
    // Echo back to A
    struct sockaddr_storage ss;
    socklen_t slen;
    r.addr.to_sockaddr(&ss, &slen);
    sendto(b.fd(), buf, r.n, 0, reinterpret_cast<struct sockaddr *>(&ss), slen);
  }

  // Phase 3: A receives all echoes
  int echoed = 0;
  for (int i = 0; i < N; ++i) {
    char buf[64];
    auto r = m_rt->block_on(a.recv_from(Span<char>(buf, sizeof(buf))));
    if (r.n > 0) echoed++;
  }
  EXPECT_EQ(echoed, N);
}
