/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * addr.h - IPv4/IPv6 address and socket address types.
 *
 * Mirrors Rust's std::net::{IpAddr, Ipv4Addr, Ipv6Addr,
 * SocketAddr, SocketAddrV4, SocketAddrV6}. All trivially copyable
 * types store bytes in network byte order to match struct in_addr /
 * struct in6_addr layout.
 *
 * C++11-compatible.
 */

#ifndef XPP_NET_ADDR_H
#define XPP_NET_ADDR_H

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/span.h>
#include <xpp/string.h>
#include <xpp/variant.h>

#include <cstdint>
#include <cstring>
#include <sys/socket.h>

struct sockaddr;
struct sockaddr_storage;

namespace xpp {
namespace net {

/* ── AddrParseError ────────────────────────────────────────────────── */

/**
 * @brief Error type for address parsing operations.
 *
 * Typed error (Tokio-style) — self-describing, no need to compare
 * against integer codes.  Use as Result<T, AddrParseError>.
 */
enum class AddrParseError : uint8_t {
  InvalidIpv4,
  InvalidIpv6,
  InvalidPort,
  InvalidFormat, // generic fallback
};

/** @brief Human-readable message for an AddrParseError. */
const char *addr_error_message(AddrParseError e) noexcept;

/* ── Forward declarations ──────────────────────────────────────────── */

class Ipv6Addr;
class IpAddr;
class SocketAddrV4;
class SocketAddrV6;
class SocketAddr;

/* ── Ipv4Addr ──────────────────────────────────────────────────────── */

class Ipv4Addr {
public:
  static const Ipv4Addr LOCALHOST;
  static const Ipv4Addr UNSPECIFIED;
  static const Ipv4Addr BROADCAST;

  static constexpr Ipv4Addr from(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept {
    Ipv4Addr r;
    r.m_addr = (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
    return r;
  }

  static constexpr Ipv4Addr from_octets(const uint8_t o[4]) noexcept {
    return from(o[0], o[1], o[2], o[3]);
  }

  static Result<Ipv4Addr, AddrParseError> parse(Span<const char> s);
  static Result<Ipv4Addr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static Result<Ipv4Addr, AddrParseError> parse(const String &s) {
    return parse(s.as_span());
  }

  struct Octets {
    uint8_t data[4];
  };

  constexpr Octets octets() const noexcept {
    return Octets{{static_cast<uint8_t>(m_addr >> 24), static_cast<uint8_t>(m_addr >> 16),
                   static_cast<uint8_t>(m_addr >> 8), static_cast<uint8_t>(m_addr)}};
  }

  constexpr uint8_t operator[](size_t i) const noexcept {
    return static_cast<uint8_t>(m_addr >> (24 - i * 8));
  }

  constexpr bool is_loopback() const noexcept {
    return (m_addr >> 24) == 127;
  }

  constexpr bool is_unspecified() const noexcept {
    return m_addr == 0;
  }

  constexpr bool is_multicast() const noexcept {
    return (m_addr >> 24) >= 224 && (m_addr >> 24) <= 239;
  }

  constexpr bool is_link_local() const noexcept {
    return (m_addr >> 16) == (169 << 8 | 254);
  }

  constexpr bool is_broadcast() const noexcept {
    return m_addr == 0xFFFFFFFF;
  }

  constexpr bool is_private() const noexcept {
    return (m_addr >> 24) == 10 || ((m_addr >> 24) == 172 && ((m_addr >> 16) & 0xF0) == 16) ||
           ((m_addr >> 24) == 192 && ((m_addr >> 16) & 0xFF) == 168);
  }

  Ipv6Addr to_ipv6_mapped() const noexcept;
  Ipv6Addr to_ipv6_compatible() const noexcept;

  String to_string() const;

  constexpr bool operator==(Ipv4Addr o) const noexcept {
    return m_addr == o.m_addr;
  }

  constexpr bool operator!=(Ipv4Addr o) const noexcept {
    return m_addr != o.m_addr;
  }

  constexpr bool operator<(Ipv4Addr o) const noexcept {
    return m_addr < o.m_addr;
  }

private:
  uint32_t m_addr; // host byte order (big-endian semantics: a.b.c.d → a<<24|b<<16|c<<8|d)

  constexpr Ipv4Addr() noexcept : m_addr(0) {}
};

/* ── Ipv6Addr ──────────────────────────────────────────────────────── */

class Ipv6Addr {
public:
  static const Ipv6Addr LOCALHOST;
  static const Ipv6Addr UNSPECIFIED;

  static constexpr Ipv6Addr from(uint16_t s0, uint16_t s1, uint16_t s2, uint16_t s3, uint16_t s4,
                                 uint16_t s5, uint16_t s6, uint16_t s7) noexcept {
    return Ipv6Addr(s0, s1, s2, s3, s4, s5, s6, s7);
  }

  static Result<Ipv6Addr, AddrParseError> parse(Span<const char> s);
  static Result<Ipv6Addr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static Result<Ipv6Addr, AddrParseError> parse(const String &s) {
    return parse(s.as_span());
  }

  struct Segments {
    uint16_t data[8];
  };

  Segments segments() const noexcept;

  struct Octets {
    uint8_t data[16];
  };

  constexpr Octets octets() const noexcept {
    return Octets{{m_octets[0], m_octets[1], m_octets[2], m_octets[3], m_octets[4], m_octets[5],
                   m_octets[6], m_octets[7], m_octets[8], m_octets[9], m_octets[10], m_octets[11],
                   m_octets[12], m_octets[13], m_octets[14], m_octets[15]}};
  }

  constexpr bool is_loopback() const noexcept {
    return m_octets[0] == 0 && m_octets[1] == 0 && m_octets[2] == 0 && m_octets[3] == 0 &&
           m_octets[4] == 0 && m_octets[5] == 0 && m_octets[6] == 0 && m_octets[7] == 0 &&
           m_octets[8] == 0 && m_octets[9] == 0 && m_octets[10] == 0 && m_octets[11] == 0 &&
           m_octets[12] == 0 && m_octets[13] == 0 && m_octets[14] == 0 && m_octets[15] == 1;
  }

  constexpr bool is_unspecified() const noexcept {
    return m_octets[0] == 0 && m_octets[1] == 0 && m_octets[2] == 0 && m_octets[3] == 0 &&
           m_octets[4] == 0 && m_octets[5] == 0 && m_octets[6] == 0 && m_octets[7] == 0 &&
           m_octets[8] == 0 && m_octets[9] == 0 && m_octets[10] == 0 && m_octets[11] == 0 &&
           m_octets[12] == 0 && m_octets[13] == 0 && m_octets[14] == 0 && m_octets[15] == 0;
  }

  constexpr bool is_multicast() const noexcept {
    return m_octets[0] == 0xFF;
  }

  constexpr bool is_ipv4_mapped() const noexcept {
    return m_octets[0] == 0 && m_octets[1] == 0 && m_octets[2] == 0 && m_octets[3] == 0 &&
           m_octets[4] == 0 && m_octets[5] == 0 && m_octets[6] == 0 && m_octets[7] == 0 &&
           m_octets[8] == 0 && m_octets[9] == 0 && m_octets[10] == 0xFF && m_octets[11] == 0xFF;
  }

  constexpr bool is_ipv4_compatible() const noexcept {
    return m_octets[0] == 0 && m_octets[1] == 0 && m_octets[2] == 0 && m_octets[3] == 0 &&
           m_octets[4] == 0 && m_octets[5] == 0 && m_octets[6] == 0 && m_octets[7] == 0 &&
           m_octets[8] == 0 && m_octets[9] == 0 && m_octets[10] == 0 && m_octets[11] == 0 &&
           !(m_octets[12] == 0 && m_octets[13] == 0 && m_octets[14] == 0 && m_octets[15] == 0);
  }

  Option<Ipv4Addr> to_ipv4() const noexcept;

  String to_string() const;

  bool operator==(Ipv6Addr o) const noexcept {
    return memcmp(m_octets, o.m_octets, 16) == 0;
  }

  bool operator!=(Ipv6Addr o) const noexcept {
    return !(*this == o);
  }

  bool operator<(Ipv6Addr o) const noexcept {
    return memcmp(m_octets, o.m_octets, 16) < 0;
  }

private:
  uint8_t m_octets[16];

  explicit constexpr Ipv6Addr(uint16_t s0, uint16_t s1, uint16_t s2, uint16_t s3, uint16_t s4,
                              uint16_t s5, uint16_t s6, uint16_t s7) noexcept
      : m_octets{static_cast<uint8_t>(s0 >> 8), static_cast<uint8_t>(s0),
                 static_cast<uint8_t>(s1 >> 8), static_cast<uint8_t>(s1),
                 static_cast<uint8_t>(s2 >> 8), static_cast<uint8_t>(s2),
                 static_cast<uint8_t>(s3 >> 8), static_cast<uint8_t>(s3),
                 static_cast<uint8_t>(s4 >> 8), static_cast<uint8_t>(s4),
                 static_cast<uint8_t>(s5 >> 8), static_cast<uint8_t>(s5),
                 static_cast<uint8_t>(s6 >> 8), static_cast<uint8_t>(s6),
                 static_cast<uint8_t>(s7 >> 8), static_cast<uint8_t>(s7)} {}

  friend class Ipv4Addr;
};

/* ── IpAddr ────────────────────────────────────────────────────────── */

class IpAddr {
public:
  static IpAddr from(Ipv4Addr addr) noexcept {
    return IpAddr(Variant<Ipv4Addr, Ipv6Addr>(addr));
  }

  static IpAddr from(Ipv6Addr addr) noexcept {
    return IpAddr(Variant<Ipv4Addr, Ipv6Addr>(addr));
  }

  static Result<IpAddr, AddrParseError> parse(Span<const char> s);
  static Result<IpAddr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static Result<IpAddr, AddrParseError> parse(const String &s) {
    return parse(s.as_span());
  }

  bool is_ipv4() const noexcept {
    return m_data.index() == 0;
  }

  bool is_ipv6() const noexcept {
    return m_data.index() == 1;
  }

  Ipv4Addr &as_ipv4() {
    return m_data.get<Ipv4Addr>();
  }
  const Ipv4Addr &as_ipv4() const {
    return m_data.get<Ipv4Addr>();
  }
  Ipv6Addr &as_ipv6() {
    return m_data.get<Ipv6Addr>();
  }
  const Ipv6Addr &as_ipv6() const {
    return m_data.get<Ipv6Addr>();
  }

  Option<Ipv4Addr> ipv4() const {
    if (is_ipv4()) return Some(m_data.get_unchecked<Ipv4Addr>());
    return none;
  }

  Option<Ipv6Addr> ipv6() const {
    if (is_ipv6()) return Some(m_data.get_unchecked<Ipv6Addr>());
    return none;
  }

  String to_string() const;

  bool operator==(const IpAddr &o) const noexcept {
    if (m_data.index() != o.m_data.index()) return false;
    return is_ipv4() ? as_ipv4() == o.as_ipv4() : as_ipv6() == o.as_ipv6();
  }

  bool operator!=(const IpAddr &o) const noexcept {
    return !(*this == o);
  }

private:
  Variant<Ipv4Addr, Ipv6Addr> m_data;

  explicit IpAddr(Variant<Ipv4Addr, Ipv6Addr> data) noexcept : m_data(std::move(data)) {}
};

/* ── SocketAddrV4 ──────────────────────────────────────────────────── */

class SocketAddrV4 {
public:
  static constexpr SocketAddrV4 from(Ipv4Addr ip, uint16_t port) noexcept {
    return SocketAddrV4(ip, port);
  }

  static Result<SocketAddrV4, AddrParseError> parse(Span<const char> s);
  static Result<SocketAddrV4, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static Result<SocketAddrV4, AddrParseError> parse(const String &s) {
    return parse(s.as_span());
  }

  constexpr Ipv4Addr ip() const noexcept {
    return m_ip;
  }
  constexpr uint16_t port() const noexcept {
    return m_port;
  }

  void set_ip(Ipv4Addr ip) noexcept {
    m_ip = ip;
  }
  void set_port(uint16_t port) noexcept {
    m_port = port;
  }

  String to_string() const;

  constexpr bool operator==(SocketAddrV4 o) const noexcept {
    return m_ip == o.m_ip && m_port == o.m_port;
  }

  constexpr bool operator!=(SocketAddrV4 o) const noexcept {
    return !(*this == o);
  }

private:
  Ipv4Addr m_ip;
  uint16_t m_port;

  constexpr SocketAddrV4(Ipv4Addr ip, uint16_t port) noexcept : m_ip(ip), m_port(port) {}
};

/* ── SocketAddrV6 ──────────────────────────────────────────────────── */

class SocketAddrV6 {
public:
  static constexpr SocketAddrV6 from(Ipv6Addr ip, uint16_t port, uint32_t flowinfo,
                                     uint32_t scope_id) noexcept {
    return SocketAddrV6(ip, port, flowinfo, scope_id);
  }

  static Result<SocketAddrV6, AddrParseError> parse(Span<const char> s);
  static Result<SocketAddrV6, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static Result<SocketAddrV6, AddrParseError> parse(const String &s) {
    return parse(s.as_span());
  }

  constexpr Ipv6Addr ip() const noexcept {
    return m_ip;
  }
  constexpr uint16_t port() const noexcept {
    return m_port;
  }
  constexpr uint32_t flowinfo() const noexcept {
    return m_flowinfo;
  }
  constexpr uint32_t scope_id() const noexcept {
    return m_scope_id;
  }

  void set_ip(Ipv6Addr ip) noexcept {
    m_ip = ip;
  }
  void set_port(uint16_t port) noexcept {
    m_port = port;
  }
  void set_flowinfo(uint32_t fi) noexcept {
    m_flowinfo = fi;
  }
  void set_scope_id(uint32_t sid) noexcept {
    m_scope_id = sid;
  }

  String to_string() const;

  bool operator==(SocketAddrV6 o) const noexcept {
    return m_ip == o.m_ip && m_port == o.m_port && m_flowinfo == o.m_flowinfo &&
           m_scope_id == o.m_scope_id;
  }

  bool operator!=(SocketAddrV6 o) const noexcept {
    return !(*this == o);
  }

private:
  Ipv6Addr m_ip;
  uint16_t m_port;
  uint32_t m_flowinfo;
  uint32_t m_scope_id;

  constexpr SocketAddrV6(Ipv6Addr ip, uint16_t port, uint32_t flowinfo, uint32_t scope_id) noexcept
      : m_ip(ip), m_port(port), m_flowinfo(flowinfo), m_scope_id(scope_id) {}
};

/* ── SocketAddr ────────────────────────────────────────────────────── */

class SocketAddr {
public:
  static SocketAddr from(SocketAddrV4 addr) noexcept {
    return SocketAddr(Variant<SocketAddrV4, SocketAddrV6>(addr));
  }

  static SocketAddr from(SocketAddrV6 addr) noexcept {
    return SocketAddr(Variant<SocketAddrV4, SocketAddrV6>(addr));
  }

  /// IPv4 wildcard address 0.0.0.0:0 — used as a sentinel and as a
  /// generic "bind to anything" target.
  static SocketAddr unspecified() noexcept {
    return SocketAddr::from(SocketAddrV4::from(Ipv4Addr::UNSPECIFIED, 0));
  }

  static Result<SocketAddr, AddrParseError> parse(Span<const char> s);
  static Result<SocketAddr, AddrParseError> parse(const char *s) {
    return parse(Span<const char>(s, strlen(s)));
  }
  static Result<SocketAddr, AddrParseError> parse(const String &s) {
    return parse(s.as_span());
  }

  bool is_ipv4() const noexcept {
    return m_data.index() == 0;
  }
  bool is_ipv6() const noexcept {
    return m_data.index() == 1;
  }

  SocketAddrV4 &as_v4() {
    return m_data.get<SocketAddrV4>();
  }
  const SocketAddrV4 &as_v4() const {
    return m_data.get<SocketAddrV4>();
  }
  SocketAddrV6 &as_v6() {
    return m_data.get<SocketAddrV6>();
  }
  const SocketAddrV6 &as_v6() const {
    return m_data.get<SocketAddrV6>();
  }

  IpAddr   ip() const;
  uint16_t port() const;

  Option<SocketAddrV4> v4() const {
    if (is_ipv4()) return Some(m_data.get_unchecked<SocketAddrV4>());
    return none;
  }

  Option<SocketAddrV6> v6() const {
    if (is_ipv6()) return Some(m_data.get_unchecked<SocketAddrV6>());
    return none;
  }

  String to_string() const;

  bool operator==(const SocketAddr &o) const noexcept {
    if (m_data.index() != o.m_data.index()) return false;
    return is_ipv4() ? as_v4() == o.as_v4() : as_v6() == o.as_v6();
  }

  bool operator!=(const SocketAddr &o) const noexcept {
    return !(*this == o);
  }

  /* sockaddr interop — implemented in addr.cpp */
  static Option<SocketAddr> from_sockaddr(const struct sockaddr *sa, socklen_t len);
  void                      to_sockaddr(struct sockaddr_storage *out, socklen_t *out_len) const;

private:
  Variant<SocketAddrV4, SocketAddrV6> m_data;

  explicit SocketAddr(Variant<SocketAddrV4, SocketAddrV6> data) noexcept
      : m_data(std::move(data)) {}
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_ADDR_H
