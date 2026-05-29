/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * socket.h - Shared socket types for net subsystem.
 *
 * Extracted from udp.h so both TCP and UDP can share these definitions
 * without a circular include dependency.
 *
 * C++11-compatible.
 */

#ifndef XPP_NET_SOCKET_H
#define XPP_NET_SOCKET_H

#include <cstdint>

namespace xpp {
namespace net {

/* ── SocketError ───────────────────────────────────────────────────── */

enum class SocketError : uint8_t {
  CreateFailed,
  BindFailed,
  ConnectFailed,
  AcceptFailed,
  AddrFamilyMismatch,
  ResolveFailed,    ///< DNS resolution failed
  NoAddress,        ///< Resolution returned zero usable addresses
};

const char *socket_error_message(SocketError e) noexcept;

/// Create a non-blocking, CLOEXEC socket.  On Linux, uses SOCK_CLOEXEC
/// to avoid the fork-race window inherent in fcntl(FD_CLOEXEC).
/// Returns -1 on failure (errno set).
int create_socket(int family, int type);

/* ── Shutdown ──────────────────────────────────────────────────────── */

enum class Shutdown : uint8_t {
  Read,  ///< SHUT_RD — close read half
  Write, ///< SHUT_WR — close write half
  Both,  ///< SHUT_RDWR — close both halves
};

} // namespace net
} // namespace xpp

#endif // XPP_NET_SOCKET_H
