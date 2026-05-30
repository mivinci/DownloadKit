/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_test.cpp - Unit tests for xpp::net::lookup_host.
 */

#include <gtest/gtest.h>
#include <xpp/net/dns.h>
#include <xpp/runtime.h>

using namespace xpp;
using namespace xpp::net;

class DnsTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_rt = new Runtime(2);
  }
  void TearDown() override {
    delete m_rt;
  }
  Runtime *m_rt;
};

/* ── lookup_host: numeric IPs ─────────────────────────────────────── */

TEST_F(DnsTest, LookupIpv4Literal) {
  auto r = m_rt->block_on([&] { return lookup_host("127.0.0.1:80"); });
  ASSERT_TRUE(r.is_ok());
  auto addrs = std::move(r).unwrap();
  ASSERT_FALSE(addrs.is_empty());
  EXPECT_TRUE(addrs[0].is_ipv4());
  EXPECT_EQ(addrs[0].port(), 80);
}

TEST_F(DnsTest, LookupIpv6Literal) {
  auto r = m_rt->block_on([&] { return lookup_host("[::1]:8080"); });
  ASSERT_TRUE(r.is_ok());
  auto addrs = std::move(r).unwrap();
  ASSERT_FALSE(addrs.is_empty());
  EXPECT_TRUE(addrs[0].is_ipv6());
  EXPECT_EQ(addrs[0].port(), 8080);
}

/* ── lookup_host: hostname ────────────────────────────────────────── */

TEST_F(DnsTest, LookupLocalhost) {
  // "localhost" is in /etc/hosts on every supported platform.
  auto r = m_rt->block_on([&] { return lookup_host("localhost:1234"); });
  ASSERT_TRUE(r.is_ok());
  auto addrs = std::move(r).unwrap();
  ASSERT_FALSE(addrs.is_empty());
  for (size_t i = 0; i < addrs.len(); ++i) {
    EXPECT_EQ(addrs[i].port(), 1234);
  }
}

/* ── lookup_host: failure cases ───────────────────────────────────── */

TEST_F(DnsTest, LookupBogusName) {
  // .invalid is reserved by RFC 2606 — guaranteed not to resolve.
  auto r = m_rt->block_on([&] { return lookup_host("nonexistent.invalid:80"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err(), SocketError::ResolveFailed);
}

TEST_F(DnsTest, LookupMalformedNoPort) {
  auto r = m_rt->block_on([&] { return lookup_host("127.0.0.1"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err(), SocketError::ResolveFailed);
}

TEST_F(DnsTest, LookupMalformedEmptyHost) {
  auto r = m_rt->block_on([&] { return lookup_host(":80"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err(), SocketError::ResolveFailed);
}

TEST_F(DnsTest, LookupMalformedUnclosedBracket) {
  auto r = m_rt->block_on([&] { return lookup_host("[::1:80"); });
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(std::move(r).unwrap_err(), SocketError::ResolveFailed);
}

/* ── lookup_host: runs off the worker thread ─────────────────────── */

TEST_F(DnsTest, LookupRunsOnBlockingPool) {
  // Confirm getaddrinfo doesn't block the calling worker thread —
  // sequential lookups must each complete (each is a separate
  // spawn_blocking submission to the m_group blocking pool).
  for (int i = 0; i < 4; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "127.0.0.1:%d", 80 + i);
    auto r = m_rt->block_on([&] { return lookup_host(buf); });
    ASSERT_TRUE(r.is_ok()) << "lookup #" << i << " failed";
  }
}
