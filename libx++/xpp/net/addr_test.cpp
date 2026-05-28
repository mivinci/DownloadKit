/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * addr_test.cpp - Unit tests for xpp::net address types.
 */

#include <gtest/gtest.h>
#include <xpp/net/addr.h>

#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace xpp;
using namespace xpp::net;

/* ── Ipv4Addr ──────────────────────────────────────────────────────── */

TEST(Ipv4AddrTest, FromOctets) {
  auto a = Ipv4Addr::from(192, 168, 1, 1);
  EXPECT_EQ(a[0], 192);
  EXPECT_EQ(a[1], 168);
  EXPECT_EQ(a[2], 1);
  EXPECT_EQ(a[3], 1);
}

TEST(Ipv4AddrTest, Constants) {
  EXPECT_EQ(Ipv4Addr::LOCALHOST, Ipv4Addr::from(127, 0, 0, 1));
  EXPECT_EQ(Ipv4Addr::UNSPECIFIED, Ipv4Addr::from(0, 0, 0, 0));
  EXPECT_EQ(Ipv4Addr::BROADCAST, Ipv4Addr::from(255, 255, 255, 255));
}

TEST(Ipv4AddrTest, Octets) {
  auto a = Ipv4Addr::from(10, 20, 30, 40);
  auto o = a.octets();
  EXPECT_EQ(o.data[0], 10);
  EXPECT_EQ(o.data[1], 20);
  EXPECT_EQ(o.data[2], 30);
  EXPECT_EQ(o.data[3], 40);
}

TEST(Ipv4AddrTest, ParseValid) {
  auto a = Ipv4Addr::parse(Span<const char>("127.0.0.1", 9));
  ASSERT_TRUE(a.is_ok());
  EXPECT_EQ(a.unwrap(), Ipv4Addr::LOCALHOST);

  auto b = Ipv4Addr::parse(Span<const char>("0.0.0.0", 7));
  ASSERT_TRUE(b.is_ok());
  EXPECT_EQ(b.unwrap(), Ipv4Addr::UNSPECIFIED);

  auto c = Ipv4Addr::parse(Span<const char>("255.255.255.255", 15));
  ASSERT_TRUE(c.is_ok());
  EXPECT_EQ(c.unwrap(), Ipv4Addr::BROADCAST);
}

TEST(Ipv4AddrTest, ParseInvalid) {
  EXPECT_TRUE(Ipv4Addr::parse(Span<const char>("", 0)).is_err());
  EXPECT_TRUE(Ipv4Addr::parse(Span<const char>("abc", 3)).is_err());
  EXPECT_TRUE(Ipv4Addr::parse(Span<const char>("999.0.0.0", 9)).is_err());
  EXPECT_TRUE(Ipv4Addr::parse(Span<const char>("1.2.3", 5)).is_err());
  EXPECT_TRUE(Ipv4Addr::parse(Span<const char>("1.2.3.4.5", 9)).is_err());
}

TEST(Ipv4AddrTest, QueryLoopback) {
  EXPECT_TRUE(Ipv4Addr::from(127, 0, 0, 1).is_loopback());
  EXPECT_TRUE(Ipv4Addr::from(127, 255, 255, 255).is_loopback());
  EXPECT_FALSE(Ipv4Addr::from(192, 168, 1, 1).is_loopback());
}

TEST(Ipv4AddrTest, QueryUnspecified) {
  EXPECT_TRUE(Ipv4Addr::UNSPECIFIED.is_unspecified());
  EXPECT_FALSE(Ipv4Addr::LOCALHOST.is_unspecified());
}

TEST(Ipv4AddrTest, QueryMulticast) {
  EXPECT_TRUE(Ipv4Addr::from(224, 0, 0, 1).is_multicast());
  EXPECT_TRUE(Ipv4Addr::from(239, 255, 255, 255).is_multicast());
  EXPECT_FALSE(Ipv4Addr::LOCALHOST.is_multicast());
}

TEST(Ipv4AddrTest, QueryBroadcast) {
  EXPECT_TRUE(Ipv4Addr::BROADCAST.is_broadcast());
  EXPECT_FALSE(Ipv4Addr::LOCALHOST.is_broadcast());
}

TEST(Ipv4AddrTest, QueryPrivate) {
  EXPECT_TRUE(Ipv4Addr::from(10, 0, 0, 1).is_private());
  EXPECT_TRUE(Ipv4Addr::from(172, 16, 0, 1).is_private());
  EXPECT_TRUE(Ipv4Addr::from(192, 168, 1, 1).is_private());
  EXPECT_FALSE(Ipv4Addr::from(8, 8, 8, 8).is_private());
}

TEST(Ipv4AddrTest, QueryLinkLocal) {
  EXPECT_TRUE(Ipv4Addr::from(169, 254, 0, 1).is_link_local());
  EXPECT_FALSE(Ipv4Addr::LOCALHOST.is_link_local());
}

TEST(Ipv4AddrTest, ToString) {
  EXPECT_STREQ(Ipv4Addr::from(127, 0, 0, 1).to_string().c_str(), "127.0.0.1");
  EXPECT_STREQ(Ipv4Addr::from(0, 0, 0, 0).to_string().c_str(), "0.0.0.0");
}

TEST(Ipv4AddrTest, ToStringRoundTrip) {
  auto orig   = Ipv4Addr::from(192, 168, 1, 1);
  auto s      = orig.to_string();
  auto parsed = Ipv4Addr::parse(s.as_span());
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.unwrap(), orig);
}

TEST(Ipv4AddrTest, ToIpv6Mapped) {
  auto a  = Ipv4Addr::from(192, 168, 1, 1);
  auto v6 = a.to_ipv6_mapped();
  EXPECT_TRUE(v6.is_ipv4_mapped());
  EXPECT_STREQ(v6.to_string().c_str(), "::ffff:192.168.1.1");
}

TEST(Ipv4AddrTest, Comparison) {
  auto a = Ipv4Addr::from(1, 2, 3, 4);
  auto b = Ipv4Addr::from(1, 2, 3, 4);
  auto c = Ipv4Addr::from(1, 2, 3, 5);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_TRUE(a < c);
}

TEST(Ipv4AddrTest, Size) {
  EXPECT_EQ(sizeof(Ipv4Addr), 4u);
}

/* ── Ipv6Addr ──────────────────────────────────────────────────────── */

TEST(Ipv6AddrTest, FromSegments) {
  auto a = Ipv6Addr::from(0, 0, 0, 0, 0, 0, 0, 1);
  EXPECT_TRUE(a.is_loopback());
  EXPECT_STREQ(a.to_string().c_str(), "::1");
}

TEST(Ipv6AddrTest, Constants) {
  EXPECT_TRUE(Ipv6Addr::LOCALHOST.is_loopback());
  EXPECT_TRUE(Ipv6Addr::UNSPECIFIED.is_unspecified());
  EXPECT_STREQ(Ipv6Addr::UNSPECIFIED.to_string().c_str(), "::");
}

TEST(Ipv6AddrTest, Segments) {
  auto a = Ipv6Addr::from(0x2001, 0x0db8, 0, 0, 0, 0, 0, 1);
  auto s = a.segments();
  EXPECT_EQ(s.data[0], 0x2001);
  EXPECT_EQ(s.data[1], 0x0db8);
  EXPECT_EQ(s.data[7], 1);
}

TEST(Ipv6AddrTest, ParseValid) {
  auto a = Ipv6Addr::parse(Span<const char>("::1", 3));
  ASSERT_TRUE(a.is_ok());
  EXPECT_EQ(a.unwrap(), Ipv6Addr::LOCALHOST);

  auto b = Ipv6Addr::parse(Span<const char>("::", 2));
  ASSERT_TRUE(b.is_ok());
  EXPECT_EQ(b.unwrap(), Ipv6Addr::UNSPECIFIED);

  auto c = Ipv6Addr::parse(Span<const char>("fe80::1", 7));
  ASSERT_TRUE(c.is_ok());
  EXPECT_STREQ(c.unwrap().to_string().c_str(), "fe80::1");

  auto d = Ipv6Addr::parse(Span<const char>("2001:0db8:0000:0000:0000:0000:0000:0001", 39));
  ASSERT_TRUE(d.is_ok());
  EXPECT_STREQ(d.unwrap().to_string().c_str(), "2001:db8::1");
}

TEST(Ipv6AddrTest, ParseInvalid) {
  EXPECT_TRUE(Ipv6Addr::parse(Span<const char>("", 0)).is_err());
  EXPECT_TRUE(Ipv6Addr::parse(Span<const char>(":", 1)).is_err());
  EXPECT_TRUE(Ipv6Addr::parse(Span<const char>("xyz", 3)).is_err());
}

TEST(Ipv6AddrTest, QueryLoopback) {
  EXPECT_TRUE(Ipv6Addr::LOCALHOST.is_loopback());
  EXPECT_FALSE(Ipv6Addr::UNSPECIFIED.is_loopback());
}

TEST(Ipv6AddrTest, QueryMulticast) {
  auto m = Ipv6Addr::from(0xff02, 0, 0, 0, 0, 0, 0, 1);
  EXPECT_TRUE(m.is_multicast());
  EXPECT_FALSE(Ipv6Addr::LOCALHOST.is_multicast());
}

TEST(Ipv6AddrTest, QueryIpv4Mapped) {
  // ::ffff:192.168.1.1
  auto a = Ipv6Addr::from(0, 0, 0, 0, 0, 0xFFFF, 0xC0A8, 0x0101);
  EXPECT_TRUE(a.is_ipv4_mapped());
  EXPECT_FALSE(Ipv6Addr::LOCALHOST.is_ipv4_mapped());
}

TEST(Ipv6AddrTest, ToIpv4Mapped) {
  auto v6 = Ipv4Addr::from(192, 168, 1, 1).to_ipv6_mapped();
  auto v4 = v6.to_ipv4();
  ASSERT_TRUE(v4.is_some());
  EXPECT_EQ(v4.unwrap(), Ipv4Addr::from(192, 168, 1, 1));
}

TEST(Ipv6AddrTest, ToIpv4None) {
  auto v4 = Ipv6Addr::LOCALHOST.to_ipv4();
  EXPECT_TRUE(v4.is_none());
}

TEST(Ipv6AddrTest, ToStringRoundTrip) {
  auto orig   = Ipv6Addr::from(0x2001, 0x0db8, 0, 0, 0, 0, 0, 1);
  auto s      = orig.to_string();
  auto parsed = Ipv6Addr::parse(s.as_span());
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.unwrap(), orig);
}

TEST(Ipv6AddrTest, Size) {
  EXPECT_EQ(sizeof(Ipv6Addr), 16u);
}

/* ── IpAddr ────────────────────────────────────────────────────────── */

TEST(IpAddrTest, FromV4) {
  auto a = IpAddr::from(Ipv4Addr::LOCALHOST);
  EXPECT_TRUE(a.is_ipv4());
  EXPECT_FALSE(a.is_ipv6());
  EXPECT_EQ(a.as_ipv4(), Ipv4Addr::LOCALHOST);
}

TEST(IpAddrTest, FromV6) {
  auto a = IpAddr::from(Ipv6Addr::LOCALHOST);
  EXPECT_TRUE(a.is_ipv6());
  EXPECT_EQ(a.as_ipv6(), Ipv6Addr::LOCALHOST);
}

TEST(IpAddrTest, SafeAccessors) {
  auto v4 = IpAddr::from(Ipv4Addr::LOCALHOST);
  EXPECT_TRUE(v4.ipv4().is_some());
  EXPECT_TRUE(v4.ipv6().is_none());

  auto v6 = IpAddr::from(Ipv6Addr::LOCALHOST);
  EXPECT_TRUE(v6.ipv4().is_none());
  EXPECT_TRUE(v6.ipv6().is_some());
}

TEST(IpAddrTest, ParseV4) {
  auto a = IpAddr::parse(Span<const char>("127.0.0.1", 9));
  ASSERT_TRUE(a.is_ok());
  EXPECT_TRUE(a.unwrap().is_ipv4());
}

TEST(IpAddrTest, ParseV6) {
  auto a = IpAddr::parse(Span<const char>("::1", 3));
  ASSERT_TRUE(a.is_ok());
  EXPECT_TRUE(a.unwrap().is_ipv6());
}

TEST(IpAddrTest, ToStringV4) {
  auto a = IpAddr::from(Ipv4Addr::LOCALHOST);
  EXPECT_STREQ(a.to_string().c_str(), "127.0.0.1");
}

TEST(IpAddrTest, ToStringV6) {
  auto a = IpAddr::from(Ipv6Addr::LOCALHOST);
  EXPECT_STREQ(a.to_string().c_str(), "::1");
}

TEST(IpAddrTest, Comparison) {
  auto a = IpAddr::from(Ipv4Addr::LOCALHOST);
  auto b = IpAddr::from(Ipv4Addr::LOCALHOST);
  auto c = IpAddr::from(Ipv6Addr::LOCALHOST);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

/* ── SocketAddrV4 ──────────────────────────────────────────────────── */

TEST(SocketAddrV4Test, From) {
  auto a = SocketAddrV4::from(Ipv4Addr::LOCALHOST, 8080);
  EXPECT_EQ(a.ip(), Ipv4Addr::LOCALHOST);
  EXPECT_EQ(a.port(), 8080);
}

TEST(SocketAddrV4Test, Setters) {
  auto a = SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80);
  a.set_port(443);
  EXPECT_EQ(a.port(), 443);
  a.set_ip(Ipv4Addr::UNSPECIFIED);
  EXPECT_EQ(a.ip(), Ipv4Addr::UNSPECIFIED);
}

TEST(SocketAddrV4Test, ParseValid) {
  auto a = SocketAddrV4::parse(Span<const char>("127.0.0.1:8080", 14));
  ASSERT_TRUE(a.is_ok());
  EXPECT_EQ(a.unwrap().ip(), Ipv4Addr::LOCALHOST);
  EXPECT_EQ(a.unwrap().port(), 8080);
}

TEST(SocketAddrV4Test, ParseZeroPort) {
  auto a = SocketAddrV4::parse(Span<const char>("0.0.0.0:0", 9));
  ASSERT_TRUE(a.is_ok());
  EXPECT_EQ(a.unwrap().ip(), Ipv4Addr::UNSPECIFIED);
  EXPECT_EQ(a.unwrap().port(), 0);
}

TEST(SocketAddrV4Test, ParseInvalid) {
  EXPECT_TRUE(SocketAddrV4::parse(Span<const char>("", 0)).is_err());
  EXPECT_TRUE(SocketAddrV4::parse(Span<const char>("127.0.0.1", 9)).is_err());
  EXPECT_TRUE(SocketAddrV4::parse(Span<const char>("127.0.0.1:99999", 15)).is_err());
  EXPECT_TRUE(SocketAddrV4::parse(Span<const char>("abc:def", 7)).is_err());
}

TEST(SocketAddrV4Test, ToString) {
  auto a = SocketAddrV4::from(Ipv4Addr::LOCALHOST, 8080);
  EXPECT_STREQ(a.to_string().c_str(), "127.0.0.1:8080");
}

TEST(SocketAddrV4Test, ToStringRoundTrip) {
  auto orig   = SocketAddrV4::from(Ipv4Addr::from(10, 0, 0, 1), 443);
  auto s      = orig.to_string();
  auto parsed = SocketAddrV4::parse(s.as_span());
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.unwrap(), orig);
}

TEST(SocketAddrV4Test, Comparison) {
  auto a = SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80);
  auto b = SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80);
  auto c = SocketAddrV4::from(Ipv4Addr::LOCALHOST, 443);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

/* ── SocketAddrV6 ──────────────────────────────────────────────────── */

TEST(SocketAddrV6Test, From) {
  auto a = SocketAddrV6::from(Ipv6Addr::LOCALHOST, 8080, 0, 0);
  EXPECT_EQ(a.ip(), Ipv6Addr::LOCALHOST);
  EXPECT_EQ(a.port(), 8080);
  EXPECT_EQ(a.flowinfo(), 0u);
  EXPECT_EQ(a.scope_id(), 0u);
}

TEST(SocketAddrV6Test, Setters) {
  auto a = SocketAddrV6::from(Ipv6Addr::LOCALHOST, 80, 0, 0);
  a.set_port(443);
  EXPECT_EQ(a.port(), 443);
  a.set_flowinfo(42);
  EXPECT_EQ(a.flowinfo(), 42u);
  a.set_scope_id(7);
  EXPECT_EQ(a.scope_id(), 7u);
}

TEST(SocketAddrV6Test, ParseValid) {
  auto a = SocketAddrV6::parse(Span<const char>("[::1]:8080", 10));
  ASSERT_TRUE(a.is_ok());
  EXPECT_EQ(a.unwrap().ip(), Ipv6Addr::LOCALHOST);
  EXPECT_EQ(a.unwrap().port(), 8080);
}

TEST(SocketAddrV6Test, ParseInvalid) {
  EXPECT_TRUE(SocketAddrV6::parse(Span<const char>("", 0)).is_err());
  EXPECT_TRUE(SocketAddrV6::parse(Span<const char>("[::1]", 5)).is_err());
  EXPECT_TRUE(SocketAddrV6::parse(Span<const char>("::1:8080", 8)).is_err());
  EXPECT_TRUE(SocketAddrV6::parse(Span<const char>("[::1]:", 6)).is_err());
}

TEST(SocketAddrV6Test, ToString) {
  auto a = SocketAddrV6::from(Ipv6Addr::LOCALHOST, 8080, 0, 0);
  EXPECT_STREQ(a.to_string().c_str(), "[::1]:8080");
}

TEST(SocketAddrV6Test, ToStringRoundTrip) {
  auto orig   = SocketAddrV6::from(Ipv6Addr::LOCALHOST, 443, 0, 0);
  auto s      = orig.to_string();
  auto parsed = SocketAddrV6::parse(s.as_span());
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.unwrap(), orig);
}

/* ── SocketAddr ────────────────────────────────────────────────────── */

TEST(SocketAddrTest, FromV4) {
  auto a = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80));
  EXPECT_TRUE(a.is_ipv4());
  EXPECT_EQ(a.port(), 80);
  EXPECT_TRUE(a.ip().is_ipv4());
}

TEST(SocketAddrTest, FromV6) {
  auto a = SocketAddr::from(SocketAddrV6::from(Ipv6Addr::LOCALHOST, 443, 0, 0));
  EXPECT_TRUE(a.is_ipv6());
  EXPECT_EQ(a.port(), 443);
  EXPECT_TRUE(a.ip().is_ipv6());
}

TEST(SocketAddrTest, ParseV4) {
  auto a = SocketAddr::parse(Span<const char>("127.0.0.1:8080", 14));
  ASSERT_TRUE(a.is_ok());
  EXPECT_TRUE(a.unwrap().is_ipv4());
  EXPECT_EQ(a.unwrap().port(), 8080);
}

TEST(SocketAddrTest, ParseV6) {
  auto a = SocketAddr::parse(Span<const char>("[::1]:8080", 10));
  ASSERT_TRUE(a.is_ok());
  EXPECT_TRUE(a.unwrap().is_ipv6());
  EXPECT_EQ(a.unwrap().port(), 8080);
}

TEST(SocketAddrTest, ParseInvalid) {
  EXPECT_TRUE(SocketAddr::parse(Span<const char>("", 0)).is_err());
  EXPECT_TRUE(SocketAddr::parse(Span<const char>("abc", 3)).is_err());
}

TEST(SocketAddrTest, SafeAccessors) {
  auto v4 = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80));
  EXPECT_TRUE(v4.v4().is_some());
  EXPECT_TRUE(v4.v6().is_none());

  auto v6 = SocketAddr::from(SocketAddrV6::from(Ipv6Addr::LOCALHOST, 80, 0, 0));
  EXPECT_TRUE(v6.v4().is_none());
  EXPECT_TRUE(v6.v6().is_some());
}

TEST(SocketAddrTest, ToStringRoundTripV4) {
  auto orig   = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::from(10, 0, 0, 1), 443));
  auto s      = orig.to_string();
  auto parsed = SocketAddr::parse(s.as_span());
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.unwrap(), orig);
}

TEST(SocketAddrTest, ToStringRoundTripV6) {
  auto orig   = SocketAddr::from(SocketAddrV6::from(Ipv6Addr::LOCALHOST, 8080, 0, 0));
  auto s      = orig.to_string();
  auto parsed = SocketAddr::parse(s.as_span());
  ASSERT_TRUE(parsed.is_ok());
  EXPECT_EQ(parsed.unwrap(), orig);
}

TEST(SocketAddrTest, Comparison) {
  auto a = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80));
  auto b = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::LOCALHOST, 80));
  auto c = SocketAddr::from(SocketAddrV6::from(Ipv6Addr::LOCALHOST, 80, 0, 0));
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

/* ── sockaddr interop ──────────────────────────────────────────────── */

TEST(SocketAddrTest, FromSockaddrV4) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port   = htons(8080);
  // 192.168.1.1 in network byte order (big-endian)
  uint8_t ip_bytes[4] = {192, 168, 1, 1};
  memcpy(&sa.sin_addr, ip_bytes, 4);

  auto a = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
  ASSERT_TRUE(a.is_some());
  EXPECT_TRUE(a.unwrap().is_ipv4());
  EXPECT_EQ(a.unwrap().port(), 8080);
  EXPECT_EQ(a.unwrap().as_v4().ip(), Ipv4Addr::from(192, 168, 1, 1));
}

TEST(SocketAddrTest, FromSockaddrV6) {
  struct sockaddr_in6 sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family   = AF_INET6;
  sa.sin6_port     = htons(443);
  sa.sin6_flowinfo = 0;
  sa.sin6_scope_id = 0;
  // ::1
  sa.sin6_addr.s6_addr[15] = 1;

  auto a = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa));
  ASSERT_TRUE(a.is_some());
  EXPECT_TRUE(a.unwrap().is_ipv6());
  EXPECT_EQ(a.unwrap().port(), 443);
  EXPECT_EQ(a.unwrap().as_v6().ip(), Ipv6Addr::LOCALHOST);
}

TEST(SocketAddrTest, FromSockaddrUnsupported) {
  struct sockaddr sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_family = AF_UNIX;
  auto a       = SocketAddr::from_sockaddr(&sa, sizeof(sa));
  EXPECT_TRUE(a.is_none());
}

TEST(SocketAddrTest, ToSockaddrV4) {
  auto addr = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::from(10, 0, 0, 1), 80));
  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  EXPECT_EQ(len, sizeof(struct sockaddr_in));
  auto *in = reinterpret_cast<struct sockaddr_in *>(&ss);
  EXPECT_EQ(in->sin_family, AF_INET);
  EXPECT_EQ(ntohs(in->sin_port), 80);
  EXPECT_EQ(memcmp(&in->sin_addr, "\x0a\x00\x00\x01", 4), 0);
}

TEST(SocketAddrTest, ToSockaddrV6) {
  auto addr = SocketAddr::from(SocketAddrV6::from(Ipv6Addr::LOCALHOST, 443, 0, 0));
  struct sockaddr_storage ss;
  socklen_t               len;
  addr.to_sockaddr(&ss, &len);

  EXPECT_EQ(len, sizeof(struct sockaddr_in6));
  auto *in6 = reinterpret_cast<struct sockaddr_in6 *>(&ss);
  EXPECT_EQ(in6->sin6_family, AF_INET6);
  EXPECT_EQ(ntohs(in6->sin6_port), 443);
  EXPECT_EQ(in6->sin6_addr.s6_addr[15], 1);
}

TEST(SocketAddrTest, SockaddrRoundTrip) {
  auto orig = SocketAddr::from(SocketAddrV4::from(Ipv4Addr::from(192, 168, 1, 1), 8080));
  struct sockaddr_storage ss;
  socklen_t               len;
  orig.to_sockaddr(&ss, &len);

  auto back = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len);
  ASSERT_TRUE(back.is_some());
  EXPECT_EQ(back.unwrap(), orig);
}

TEST(SocketAddrTest, SockaddrRoundTripV6) {
  auto orig = SocketAddr::from(SocketAddrV6::from(Ipv6Addr::LOCALHOST, 443, 0, 0));
  struct sockaddr_storage ss;
  socklen_t               len;
  orig.to_sockaddr(&ss, &len);

  auto back = SocketAddr::from_sockaddr(reinterpret_cast<struct sockaddr *>(&ss), len);
  ASSERT_TRUE(back.is_some());
  EXPECT_EQ(back.unwrap(), orig);
}
