/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * poll_evented.h - Unified async I/O helper for non-blocking FDs.
 *
 * Generic "try + EAGAIN → wait readiness → retry" loop extracted
 * from TcpStream and UdpSocket.  Mirrors Tokio's PollEvented /
 * Registration::try_io pattern.
 *
 * The PollEvented always holds a valid fd (>= 0) and its Arc-ed
 * ScheduledIo.  The null / closed state is represented by a null
 * pointer to PollEvented in the enclosing socket type.
 *
 * C++11-compatible (header-only, templates only).
 */

#ifndef XPP_IO_POLL_EVENTED_H
#define XPP_IO_POLL_EVENTED_H

#include <xpp/arc.h>
#include <xpp/promise.h>
#include <xpp/runtime/scheduled_io.h>
#include <xpp/span.h>

#include <cerrno>
#include <fcntl.h>
#include <utility>

namespace xpp {
namespace net {

// ScheduledIo and its readiness combinators now live in xpp::runtime
// (they are part of the runtime's I/O readiness driver). Re-expose them
// here so the net layer keeps using the short names.
using runtime::ScheduledIo;
namespace _ {
using runtime::_::readable;
using runtime::_::writable;
} // namespace _

/* ── PollEvented ───────────────────────────────────────────────────── */

/**
 * @brief Async I/O wrapper for a non-blocking file descriptor.
 *
 * Always valid (fd >= 0).  Provides:
 *   - readable() / writable() — wait for readiness (direct passthrough)
 *   - async_read_op / async_write_op — try a syscall; on EAGAIN, wait
 *     the corresponding readiness direction and retry once
 *
 * Move-only.  The moved-from state is a tombstone (fd == -1, all
 * operations short-circuit).
 *
 * @code
 *   PollEvented io(fd);
 *   auto n = io.async_read_op(&::recv, fd, data, len, 0);
 *   // returns Promise<ssize_t>  (n >= 0 on success, n < 0 on error,
 *   // EAGAIN retried once via readable/writable)
 * @endcode
 */
class PollEvented {
public:
  /**
   * @brief Create a PollEvented that owns @p fd.
   *
   * Registers the fd with the current event loop for both
   * read and write events (edge-triggered).
   *
   * @pre fd must be non-blocking + CLOEXEC (use create_socket).
   * @pre fd must be >= 0.
   */
  explicit PollEvented(int fd) : m_fd(fd), m_sio(Arc<ScheduledIo>::make(fd)) {}

  /** @brief Tombstone (moved-from) constructor.  fd == -1. */
  PollEvented(std::nullptr_t) noexcept : m_fd(-1), m_sio(Arc<ScheduledIo>::make(-1)) {}

  PollEvented(PollEvented &&o) noexcept : m_fd(o.m_fd), m_sio(std::move(o.m_sio)) {
    o.m_fd = -1;
  }

  PollEvented &operator=(PollEvented &&o) noexcept {
    if (this != &o) {
      close();
      m_fd   = o.m_fd;
      m_sio  = std::move(o.m_sio);
      o.m_fd = -1;
    }
    return *this;
  }

  PollEvented(const PollEvented &)            = delete;
  PollEvented &operator=(const PollEvented &) = delete;

  ~PollEvented() {
    close();
  }

  bool operator==(std::nullptr_t) const {
    return m_fd < 0;
  }
  bool operator!=(std::nullptr_t) const {
    return m_fd >= 0;
  }

  /* ── Readiness waiters (passthrough to ScheduledIo) ─────────────── */

  /** @brief Wait until the FD is readable. */
  Promise<void> readable() const {
    return _::readable(m_sio);
  }

  /** @brief Wait until the FD is writable. */
  Promise<void> writable() const {
    return _::writable(m_sio);
  }

  /* ── Async syscall with EAGAIN retry ─────────────────────────────── */

  /**
   * @brief Try a read-direction syscall; retry once on EAGAIN.
   *
   * 1. Call @p op(@p args...).  If it returns >= 0, resolve immediately.
   * 2. If it returns < 0 and errno is EAGAIN/EWOULDBLOCK, wait for
   *    readability and retry once.
   * 3. Any other error resolves immediately with the error return.
   *
   * @p op       Callable (function pointer or lambda) taking
   *             (int fd, decayed args...) → ssize_t.
   * @p args...  Arguments forwarded to op after FD.  Decayed copies
   *             are captured into the continuation lambda.
   *
   * @warning All arguments (especially Span/pointer) are captured by
   *          value; the caller MUST keep pointed-to data alive until
   *          the returned promise resolves.
   */
  template <class Op, class... Args> Promise<ssize_t> async_read_op(Op op, Args... args) const {
    ssize_t n = op(m_fd, args...);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return Promise<ssize_t>::resolve(n);

    int  fd  = m_fd;
    auto sio = m_sio;
    return _::readable(m_sio).then([fd, sio, op, args...]() -> ssize_t { return op(fd, args...); });
  }

  /**
   * @brief Try a write-direction syscall; retry once on EAGAIN.
   *
   * Same contract as async_read_op but waits for writability.
   */
  template <class Op, class... Args> Promise<ssize_t> async_write_op(Op op, Args... args) const {
    ssize_t n = op(m_fd, args...);
    if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return Promise<ssize_t>::resolve(n);

    int  fd  = m_fd;
    auto sio = m_sio;
    return _::writable(m_sio).then([fd, sio, op, args...]() -> ssize_t { return op(fd, args...); });
  }

  /* ── FD lifecycle ────────────────────────────────────────────────── */

  /** @brief Deregister from event loop and close the kernel fd. */
  void close() {
    if (m_fd >= 0) {
      m_sio->close_fd();
      m_fd = -1;
    }
  }

  int fd() const {
    return m_fd;
  }
  bool is_closed() const {
    return m_fd < 0;
  }

  /** @brief Access the underlying ScheduledIo (for advanced callers). */
  Arc<ScheduledIo> scheduled_io() const {
    return m_sio;
  }

private:
  int              m_fd;
  Arc<ScheduledIo> m_sio;
};

} // namespace net
} // namespace xpp

#endif // XPP_IO_POLL_EVENTED_H
