/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * addr.h - IPv4/IPv6 address and socket address types.
 */

#include <xpp/net/addr.h>

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>

namespace xpp {
namespace net {

/* ── Static constants ──────────────────────────────────────────────── */

const Ipv4Addr Ipv4Addr::LOCALHOST   = Ipv4Addr::from(127, 0, 0, 1);
const Ipv4Addr Ipv4Addr::UNSPECIFIED = Ipv4Addr::from(0, 0, 0, 0);
const Ipv4Addr Ipv4Addr::BROADCAST   = Ipv4Addr::from(255, 255, 255, 255);

const Ipv6Addr Ipv6Addr::LOCALHOST   = Ipv6Addr::from(0, 0, 0, 0, 0, 0, 0, 1);
const Ipv6Addr Ipv6Addr::UNSPECIFIED = Ipv6Addr::from(0, 0, 0, 0, 0, 0, 0, 0);

/* ── Ipv4Addr ──────────────────────────────────────────────────────── */

Result<Ipv4Addr, AddrParseError> Ipv4Addr::parse(Span<const char> s) {
  if (s.is_empty()) return Result<Ipv4Addr, AddrParseError>(err, AddrParseError::InvalidIpv4);
  char   buf[16];
  size_t len = s.size() < 15 ? s.size() : 15;
  memcpy(buf, s.data(), len);
  buf[len] = '\0';
  struct in_addr addr;
  if (inet_pton(AF_INET, buf, &addr) != 1)
    return Result<Ipv4Addr, AddrParseError>(err, AddrParseError::InvalidIpv4);
  // inet_pton stores in network byte order (big-endian).
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&addr);
  return Result<Ipv4Addr, AddrParseError>(ok, Ipv4Addr::from(p[0], p[1], p[2], p[3]));
}

Ipv6Addr Ipv4Addr::to_ipv6_mapped() const noexcept {
  return Ipv6Addr::from(0, 0, 0, 0, 0, 0xFFFF, static_cast<uint16_t>(m_addr >> 16),
                        static_cast<uint16_t>(m_addr));
}

Ipv6Addr Ipv4Addr::to_ipv6_compatible() const noexcept {
  return Ipv6Addr::from(0, 0, 0, 0, 0, 0, static_cast<uint16_t>(m_addr >> 16),
                        static_cast<uint16_t>(m_addr));
}

String Ipv4Addr::to_string() const {
  char           buf[16];
  struct in_addr addr;
  uint8_t       *p = reinterpret_cast<uint8_t *>(&addr);
  p[0]             = static_cast<uint8_t>(m_addr >> 24);
  p[1]             = static_cast<uint8_t>(m_addr >> 16);
  p[2]             = static_cast<uint8_t>(m_addr >> 8);
  p[3]             = static_cast<uint8_t>(m_addr);
  inet_ntop(AF_INET, &addr, buf, sizeof(buf));
  return String::from_unchecked(buf);
}

/* ── Ipv6Addr ──────────────────────────────────────────────────────── */

Ipv6Addr::Segments Ipv6Addr::segments() const noexcept {
  Segments segs;
  for (int i = 0; i < 8; ++i)
    segs.data[i] = static_cast<uint16_t>((m_octets[i * 2] << 8) | m_octets[i * 2 + 1]);
  return segs;
}

Result<Ipv6Addr, AddrParseError> Ipv6Addr::parse(Span<const char> s) {
  if (s.is_empty()) return Result<Ipv6Addr, AddrParseError>(err, AddrParseError::InvalidIpv6);
  char   buf[48];
  size_t len = s.size() < 47 ? s.size() : 47;
  memcpy(buf, s.data(), len);
  buf[len] = '\0';
  struct in6_addr addr;
  if (inet_pton(AF_INET6, buf, &addr) != 1)
    return Result<Ipv6Addr, AddrParseError>(err, AddrParseError::InvalidIpv6);
  return Result<Ipv6Addr, AddrParseError>(
    ok, Ipv6Addr(static_cast<uint16_t>((addr.s6_addr[0] << 8) | addr.s6_addr[1]),
                 static_cast<uint16_t>((addr.s6_addr[2] << 8) | addr.s6_addr[3]),
                 static_cast<uint16_t>((addr.s6_addr[4] << 8) | addr.s6_addr[5]),
                 static_cast<uint16_t>((addr.s6_addr[6] << 8) | addr.s6_addr[7]),
                 static_cast<uint16_t>((addr.s6_addr[8] << 8) | addr.s6_addr[9]),
                 static_cast<uint16_t>((addr.s6_addr[10] << 8) | addr.s6_addr[11]),
                 static_cast<uint16_t>((addr.s6_addr[12] << 8) | addr.s6_addr[13]),
                 static_cast<uint16_t>((addr.s6_addr[14] << 8) | addr.s6_addr[15])));
}

Option<Ipv4Addr> Ipv6Addr::to_ipv4() const noexcept {
  if (!is_ipv4_mapped()) return none;
  return Some(Ipv4Addr::from(m_octets[12], m_octets[13], m_octets[14], m_octets[15]));
}

String Ipv6Addr::to_string() const {
  char            buf[48];
  struct in6_addr addr;
  memcpy(&addr, m_octets, 16);
  inet_ntop(AF_INET6, &addr, buf, sizeof(buf));
  return String::from_unchecked(buf);
}

/* ── IpAddr ────────────────────────────────────────────────────────── */

Result<IpAddr, AddrParseError> IpAddr::parse(Span<const char> s) {
  auto v4 = Ipv4Addr::parse(s);
  if (v4.is_ok()) return Result<IpAddr, AddrParseError>(ok, IpAddr::from(v4.unwrap()));
  auto v6 = Ipv6Addr::parse(s);
  if (v6.is_ok()) return Result<IpAddr, AddrParseError>(ok, IpAddr::from(v6.unwrap()));
  return Result<IpAddr, AddrParseError>(err, AddrParseError::InvalidFormat);
}

String IpAddr::to_string() const {
  return is_ipv4() ? as_ipv4().to_string() : as_ipv6().to_string();
}

/* ── SocketAddrV4 ──────────────────────────────────────────────────── */

Result<SocketAddrV4, AddrParseError> SocketAddrV4::parse(Span<const char> s) {
  // Find the last ':' — separates IP from port.
  const char *data  = s.data();
  int         len   = static_cast<int>(s.size());
  int         colon = -1;
  for (int i = len - 1; i >= 0; --i) {
    if (data[i] == ':') {
      colon = i;
      break;
    }
  }
  if (colon <= 0) return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidFormat);

  auto ip = Ipv4Addr::parse(Span<const char>(data, static_cast<size_t>(colon)));
  if (ip.is_err()) return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidIpv4);

  // Parse port.
  char port_buf[6];
  int  port_len = len - colon - 1;
  if (port_len <= 0 || port_len > 5)
    return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidPort);
  memcpy(port_buf, data + colon + 1, static_cast<size_t>(port_len));
  port_buf[port_len] = '\0';
  char *end;
  long  port = strtol(port_buf, &end, 10);
  if (*end != '\0' || port < 0 || port > 65535)
    return Result<SocketAddrV4, AddrParseError>(err, AddrParseError::InvalidPort);

  return Result<SocketAddrV4, AddrParseError>(
    ok, SocketAddrV4::from(ip.unwrap(), static_cast<uint16_t>(port)));
}

String SocketAddrV4::to_string() const {
  String s = m_ip.to_string();
  char   port_buf[8];
  snprintf(port_buf, sizeof(port_buf), ":%u", m_port);
  s.append(port_buf);
  return s;
}

/* ── SocketAddrV6 ──────────────────────────────────────────────────── */

Result<SocketAddrV6, AddrParseError> SocketAddrV6::parse(Span<const char> s) {
  // Expected form: "[addr]:port"
  const char *data = s.data();
  int         len  = static_cast<int>(s.size());
  if (len < 4 || data[0] != '[')
    return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidFormat);

  // Find "]:" delimiter.
  int delim = -1;
  for (int i = 1; i < len - 1; ++i) {
    if (data[i] == ']' && i + 1 < len && data[i + 1] == ':') {
      delim = i;
      break;
    }
  }
  if (delim < 0) return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidFormat);

  auto ip = Ipv6Addr::parse(Span<const char>(data + 1, static_cast<size_t>(delim - 1)));
  if (ip.is_err()) return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidIpv6);

  char port_buf[6];
  int  port_len = len - delim - 2;
  if (port_len <= 0 || port_len > 5)
    return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidPort);
  memcpy(port_buf, data + delim + 2, static_cast<size_t>(port_len));
  port_buf[port_len] = '\0';
  char *end;
  long  port = strtol(port_buf, &end, 10);
  if (*end != '\0' || port < 0 || port > 65535)
    return Result<SocketAddrV6, AddrParseError>(err, AddrParseError::InvalidPort);

  return Result<SocketAddrV6, AddrParseError>(
    ok, SocketAddrV6::from(ip.unwrap(), static_cast<uint16_t>(port), 0, 0));
}

String SocketAddrV6::to_string() const {
  String s("[", 1);
  s.append(m_ip.to_string());
  char port_buf[8];
  snprintf(port_buf, sizeof(port_buf), "]:%u", m_port);
  s.append(port_buf);
  return s;
}

/* ── SocketAddr ────────────────────────────────────────────────────── */

Result<SocketAddr, AddrParseError> SocketAddr::parse(Span<const char> s) {
  if (s.is_empty()) return Result<SocketAddr, AddrParseError>(err, AddrParseError::InvalidFormat);
  if (s.data()[0] == '[') {
    auto v6 = SocketAddrV6::parse(s);
    if (v6.is_ok()) return Result<SocketAddr, AddrParseError>(ok, SocketAddr::from(v6.unwrap()));
    return Result<SocketAddr, AddrParseError>(err, v6.unwrap_err());
  }
  auto v4 = SocketAddrV4::parse(s);
  if (v4.is_ok()) return Result<SocketAddr, AddrParseError>(ok, SocketAddr::from(v4.unwrap()));
  return Result<SocketAddr, AddrParseError>(err, v4.unwrap_err());
}

IpAddr SocketAddr::ip() const {
  return is_ipv4() ? IpAddr::from(as_v4().ip()) : IpAddr::from(as_v6().ip());
}

uint16_t SocketAddr::port() const {
  return is_ipv4() ? as_v4().port() : as_v6().port();
}

String SocketAddr::to_string() const {
  return is_ipv4() ? as_v4().to_string() : as_v6().to_string();
}

Option<SocketAddr> SocketAddr::from_sockaddr(const struct sockaddr *sa, socklen_t len) {
  if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
    auto          *in = reinterpret_cast<const struct sockaddr_in *>(sa);
    const uint8_t *p  = reinterpret_cast<const uint8_t *>(&in->sin_addr);
    return Some(SocketAddr::from(
      SocketAddrV4::from(Ipv4Addr::from(p[0], p[1], p[2], p[3]), ntohs(in->sin_port))));
  }
  if (sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
    auto *in6 = reinterpret_cast<const struct sockaddr_in6 *>(sa);
    return Some(SocketAddr::from(SocketAddrV6::from(
      Ipv6Addr::from(
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[0] << 8) | in6->sin6_addr.s6_addr[1]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[2] << 8) | in6->sin6_addr.s6_addr[3]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[4] << 8) | in6->sin6_addr.s6_addr[5]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[6] << 8) | in6->sin6_addr.s6_addr[7]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[8] << 8) | in6->sin6_addr.s6_addr[9]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[10] << 8) | in6->sin6_addr.s6_addr[11]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[12] << 8) | in6->sin6_addr.s6_addr[13]),
        static_cast<uint16_t>((in6->sin6_addr.s6_addr[14] << 8) | in6->sin6_addr.s6_addr[15])),
      ntohs(in6->sin6_port), ntohl(in6->sin6_flowinfo), ntohl(in6->sin6_scope_id))));
  }
  return none;
}

void SocketAddr::to_sockaddr(struct sockaddr_storage *out, socklen_t *out_len) const {
  memset(out, 0, sizeof(*out));
  if (is_ipv4()) {
    auto    *in    = reinterpret_cast<struct sockaddr_in *>(out);
    Ipv4Addr ip    = as_v4().ip();
    in->sin_family = AF_INET;
    in->sin_port   = htons(as_v4().port());
    uint8_t *p     = reinterpret_cast<uint8_t *>(&in->sin_addr);
    p[0]           = static_cast<uint8_t>(ip[0]);
    p[1]           = static_cast<uint8_t>(ip[1]);
    p[2]           = static_cast<uint8_t>(ip[2]);
    p[3]           = static_cast<uint8_t>(ip[3]);
    *out_len       = sizeof(struct sockaddr_in);
  } else {
    auto    *in6       = reinterpret_cast<struct sockaddr_in6 *>(out);
    Ipv6Addr ip        = as_v6().ip();
    in6->sin6_family   = AF_INET6;
    in6->sin6_port     = htons(as_v6().port());
    in6->sin6_flowinfo = htonl(as_v6().flowinfo());
    in6->sin6_scope_id = htonl(as_v6().scope_id());
    memcpy(&in6->sin6_addr, &ip, 16);
    *out_len = sizeof(struct sockaddr_in6);
  }
}

/* ── AddrParseError ────────────────────────────────────────────────── */

const char *addr_error_message(AddrParseError e) noexcept {
  switch (e) {
  case AddrParseError::InvalidIpv4:
    return "invalid IPv4 address";
  case AddrParseError::InvalidIpv6:
    return "invalid IPv6 address";
  case AddrParseError::InvalidPort:
    return "invalid port number";
  case AddrParseError::InvalidFormat:
    return "invalid address format";
  default:
    return "unknown address error";
  }
}

} // namespace net
} // namespace xpp
