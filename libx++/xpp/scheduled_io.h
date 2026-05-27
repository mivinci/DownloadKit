/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * scheduled_io.h - Per-FD shared readiness state with dual wakers.
 *
 * Inspired by Tokio's ScheduledIo. Each FD registered with the
 * async I/O layer gets one ScheduledIo that tracks readiness
 * independently for read and write directions, allowing concurrent
 * read() and write() on the same socket.
 *
 * C++11-compatible.
 */

#ifndef XPP_SCHEDULED_IO_H
#define XPP_SCHEDULED_IO_H

#include <xpp/promise.h>
#include <xpp/arc.h>
#include <xpp/runtime.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <unistd.h>

extern "C" {
#include <x/base/event.h>
#include <x/base/socket.h>
}

namespace xpp {

/* ── Readiness bitmask ──────────────────────────────────────────── */

namespace _ {

enum ReadyBits : uint8_t {
  kReadable = 1 << 0,
  kWritable = 1 << 1,
  kError    = 1 << 2,
  kClosed   = 1 << 3,
};

}

/* ── ScheduledIo ────────────────────────────────────────────────── */

/**
 * @brief Shared readiness state for a single FD, with independent
 *        read/write waker slots.
 *
 * Lifecycle:
 *   - Created when an FD is registered for async I/O.
 *   - Registered with xEventAdd once (Read|Write, edge-triggered).
 *   - Shared via Arc<ScheduledIo> between read and write tasks.
 *   - Deregistered + closed when the last Arc reference drops.
 */
class ScheduledIo final {
public:
  explicit ScheduledIo(int fd)
      : m_loop(current_event_loop()), m_fd(fd), m_readiness(0) {
    _::Schedule *s = nullptr;
    _::Worker *w = _::current_context().worker;
    if (w) s = &w->sched();
    m_sched.store(s, std::memory_order_release);
    m_source = xEventAdd(m_loop, fd,
                         static_cast<xEventMask>(xEvent_Read | xEvent_Write),
                         on_event, this);
  }

  ~ScheduledIo() {
    if (m_source) {
      xEventDel(m_loop, m_source);
    }
    if (m_fd >= 0) {
      close(m_fd);
    }
  }

  int fd() const { return m_fd; }
  xEventLoop loop() const { return m_loop; }

  /* ── Poll readiness (non-blocking) ──────────────────────────── */

  /**
   * @brief Check if the FD is currently readable.
   * If not, register waker for later notification.
   * Uses atomic exchange for thread safety.
   */
  bool poll_read(_::Waker waker) {
    if (m_readiness.load(std::memory_order_acquire) & _::kReadable) {
      return true;
    }
    // Atomically store the waker, then double-check readiness
    // If readiness appeared between the check and the store, we must wake immediately
    _::SpawnTaskBase *prev = m_read_waiter.exchange(waker.task(), std::memory_order_acq_rel);
    (void)prev;
    if (m_readiness.load(std::memory_order_acquire) & _::kReadable) {
      // Already readable — clear our waiter and return ready
      m_read_waiter.exchange(nullptr, std::memory_order_acq_rel);
      return true;
    }
    return false;
  }

  /**
   * @brief Check if the FD is currently writable.
   * If not, register waker for later notification.
   * Uses atomic exchange for thread safety.
   */
  bool poll_write(_::Waker waker) {
    if (m_readiness.load(std::memory_order_acquire) & _::kWritable) {
      return true;
    }
    _::SpawnTaskBase *prev = m_write_waiter.exchange(waker.task(), std::memory_order_acq_rel);
    (void)prev;
    if (m_readiness.load(std::memory_order_acquire) & _::kWritable) {
      m_write_waiter.exchange(nullptr, std::memory_order_acq_rel);
      return true;
    }
    return false;
  }

  /**
   * @brief Clear the readable readiness bit after recv() returns EAGAIN.
   */
  void clear_readable() {
    m_readiness.fetch_and(~_::kReadable, std::memory_order_acq_rel);
  }

  /**
   * @brief Clear the writable readiness bit after send() returns EAGAIN.
   */
  void clear_writable() {
    m_readiness.fetch_and(~_::kWritable, std::memory_order_acq_rel);
  }

  /**
   * @brief Close the FD and mark it as closed.
   * After this, poll_read/poll_write will return true (with zero-read/zero-write semantics handled by TcpStream).
   */
  void close_fd() {
    uint8_t prev = m_readiness.fetch_or(_::kClosed, std::memory_order_acq_rel);
    if (!(prev & _::kClosed)) {
      if (m_source) {
        xEventDel(m_loop, m_source);
        m_source = nullptr;
      }
      if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
      }
      wake_both();
    }
  }

  bool is_closed() const {
    return m_readiness.load(std::memory_order_acquire) & _::kClosed;
  }

private:
  static void on_event(int fd, xEventMask mask, void *arg) {
    (void)fd;
    auto *self = static_cast<ScheduledIo *>(arg);
    uint8_t bits = 0;
    if (mask & xEvent_Read)  bits |= _::kReadable;
    if (mask & xEvent_Write) bits |= _::kWritable;

    self->m_readiness.fetch_or(bits, std::memory_order_acq_rel);
    self->wake_both();
  }

  void wake_both() {
    _::SpawnTaskBase *reader = m_read_waiter.exchange(nullptr, std::memory_order_acq_rel);
    _::SpawnTaskBase *writer = m_write_waiter.exchange(nullptr, std::memory_order_acq_rel);
    _::Schedule *sched = m_sched.load(std::memory_order_acquire);
    if (reader && sched) sched->schedule(reader);
    if (writer && sched) sched->schedule(writer);
  }

  xEventLoop                          m_loop;
  int                                 m_fd;
  xEventSource                        m_source;
  std::atomic<uint8_t>                m_readiness;
  std::atomic<_::SpawnTaskBase *>     m_read_waiter{nullptr};
  std::atomic<_::SpawnTaskBase *>     m_write_waiter{nullptr};
  std::atomic<_::Schedule *>          m_sched{nullptr};
};

/* ── PromiseNode wrappers for poll readiness ─────────────────────── */

namespace _ {

class ReadablePromiseNode final : public PromiseNode<void> {
public:
  explicit ReadablePromiseNode(Arc<ScheduledIo> sio) : m_sio(std::move(sio)) {}

  bool poll(Waker waker) override {
    if (m_sio->poll_read(waker)) return true;
    return false;
  }

  Void take() override { return Void{}; }

private:
  Arc<ScheduledIo> m_sio;
};

class WritablePromiseNode final : public PromiseNode<void> {
public:
  explicit WritablePromiseNode(Arc<ScheduledIo> sio) : m_sio(std::move(sio)) {}

  bool poll(Waker waker) override {
    if (m_sio->poll_write(waker)) return true;
    return false;
  }

  Void take() override { return Void{}; }

private:
  Arc<ScheduledIo> m_sio;
};

inline Promise<void> readable(Arc<ScheduledIo> sio) {
  return Promise<void>(Own<PromiseNode<void>>(new ReadablePromiseNode(std::move(sio))));
}

inline Promise<void> writable(Arc<ScheduledIo> sio) {
  return Promise<void>(Own<PromiseNode<void>>(new WritablePromiseNode(std::move(sio))));
}

} // namespace _

} // namespace xpp

#endif // XPP_SCHEDULED_IO_H
