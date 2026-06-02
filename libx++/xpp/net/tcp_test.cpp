/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_test.cpp - Unit tests for xpp::net::TcpListener / TcpStream
 *                "host:port" overloads (bind / connect).
 */

#include <xpp/net/tcp.h>
#include <xpp/runtime/runtime.h>
#include <gtest/gtest.h>

#include <cstdio>

using namespace xpp;
using namespace xpp::net;

class TcpHostnameTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = new runtime::Runtime(2);
  }
  void TearDown() override {
    delete m_rt;
  }
  runtime::Runtime *m_rt;
};

/* ── TcpListener::bind("host:port") ───────────────────────────────── */

TEST_F(TcpHostnameTest, BindHostPortLoopback) {
  auto r = m_rt->block_on([&] { return TcpListener::bind("127.0.0.1:0"); });
  ASSERT_TRUE(r.is_ok());
  auto listener = std::move(r).unwrap();
  EXPECT_FALSE(listener.is_closed());
  EXPECT_NE(listener.local_addr().port(), 0);
}

TEST_F(TcpHostnameTest, BindLocalhost) {
  auto r = m_rt->block_on([&] { return TcpListener::bind("localhost:0"); });
  ASSERT_TRUE(r.is_ok());
  EXPECT_FALSE(std::move(r).unwrap().is_closed());
}

TEST_F(TcpHostnameTest, BindIpv6) {
  auto r = m_rt->block_on([&] { return TcpListener::bind("[::1]:0"); });
  ASSERT_TRUE(r.is_ok());
  auto listener = std::move(r).unwrap();
  EXPECT_TRUE(listener.local_addr().is_ipv6());
}

TEST_F(TcpHostnameTest, BindBogusHostname) {
  auto r = m_rt->block_on([&] { return TcpListener::bind("nonexistent.invalid:0"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err().kind(), io::ErrorKind::ResolveFailed);
}

/* ── TcpStream::connect("host:port") ──────────────────────────────── */

TEST_F(TcpHostnameTest, ConnectByHostPort) {
  // Bind a listener on an ephemeral port, then connect to it.
  auto bind_r = m_rt->block_on([&] { return TcpListener::bind("127.0.0.1:0"); });
  ASSERT_TRUE(bind_r.is_ok());
  auto listener = std::move(bind_r).unwrap();
  auto port     = listener.local_addr().port();

  char addr[32];
  std::snprintf(addr, sizeof(addr), "127.0.0.1:%u", static_cast<unsigned>(port));

  auto connect_r = m_rt->block_on(
    [&]() -> Promise<Result<TcpStream, io::Error>> { return TcpStream::connect(addr); });
  ASSERT_TRUE(connect_r.is_ok());
  EXPECT_FALSE(std::move(connect_r).unwrap().is_closed());
}

TEST_F(TcpHostnameTest, ConnectBogusHostname) {
  auto r = m_rt->block_on([&]() -> Promise<Result<TcpStream, io::Error>> {
    return TcpStream::connect("nonexistent.invalid:80");
  });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err().kind(), io::ErrorKind::ResolveFailed);
}

TEST_F(TcpHostnameTest, ConnectClosedPort) {
  // Port 1 (tcpmux) almost never has a listener.  The kind the loopback
  // refusal maps to is ConnectionRefused.
  auto r = m_rt->block_on(
    [&]() -> Promise<Result<TcpStream, io::Error>> { return TcpStream::connect("127.0.0.1:1"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err().kind(), io::ErrorKind::ConnectionRefused);
}

/* ── accept() — Result error model ────────────────────────────────── */

TEST_F(TcpHostnameTest, AcceptOneRoundTrip) {
  // Bind a loopback listener, fire a client connect, await accept().
  auto bind_r = m_rt->block_on([&] { return TcpListener::bind("127.0.0.1:0"); });
  ASSERT_TRUE(bind_r.is_ok());
  auto listener = std::move(bind_r).unwrap();
  auto port     = listener.local_addr().port();

  char addr[32];
  std::snprintf(addr, sizeof(addr), "127.0.0.1:%u", static_cast<unsigned>(port));

  // Drive client connect in parallel with accept on the same loop.
  auto accepted = m_rt->block_on([&]() -> Promise<int> {
    auto client_p = TcpStream::connect(addr);
    auto srv_p    = listener.accept();
    return std::move(client_p).then(
      [srv_p = std::move(srv_p)](Result<TcpStream, io::Error> cli) mutable -> Promise<int> {
        if (cli.is_err()) return Promise<int>::resolve(0);
        return std::move(srv_p).then(
          [](Result<TcpStream, io::Error> s) -> int { return s.is_ok() ? 1 : 0; });
      });
  });
  EXPECT_EQ(accepted, 1);
}

TEST_F(TcpHostnameTest, AcceptOnClosedListener) {
  auto bind_r = m_rt->block_on([&] { return TcpListener::bind("127.0.0.1:0"); });
  ASSERT_TRUE(bind_r.is_ok());
  auto listener = std::move(bind_r).unwrap();
  listener.close();

  auto r = m_rt->block_on(
    [&]() -> Promise<Result<TcpStream, io::Error>> { return listener.accept(); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err().kind(), io::ErrorKind::NotConnected);
}
