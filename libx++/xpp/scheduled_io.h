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
 * ## Waker storage
 *
 * We store the full Waker (Schedule* + SpawnTaskBase*) rather than
 * just the task pointer. This is necessary because:
 *   - CoroWakeSchedule passes task=nullptr (wakes via coroutine handle)
 *   - SyncWaitSchedule passes task=nullptr (wakes via event post)
 *   - WorkerSchedule passes the task (re-enqueues to local queue)
 * Storing only the task would lose the Schedule pointer, making the
 * first two cases inoperable.
 *
 * C++11-compatible.
 */

#ifndef XPP_SCHEDULED_IO_H
#define XPP_SCHEDULED_IO_H

#include <xpp/arc.h>
#include <xpp/promise.h>
#include <xpp/runtime.h>

#include <atomic>
#include <unistd.h>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

/* ── Readiness bitmask ──────────────────────────────────────────── */

namespace _ {

enum ReadyBits : uint8_t {
  kReadable = 1 << 0,
  kWritable = 1 << 1,
  kClosed   = 1 << 2,
};

} // namespace _

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
   *
   * If not ready, stores the waker atomically. A double-check after
   * the store prevents the lost-wake race (event arrived between
   * the initial check and the waker store).
   */
  bool poll_read(_::Waker waker) {
    if (m_readiness.load(std::memory_order_acquire) & _::kReadable) {
      return true;
    }
    set_waker(m_read_waker_sched, m_read_waker_task, waker);
    if (m_readiness.load(std::memory_order_acquire) & _::kReadable) {
      clear_waker(m_read_waker_sched, m_read_waker_task);
      return true;
    }
    return false;
  }

  /**
   * @brief Check if the FD is currently writable.
   */
  bool poll_write(_::Waker waker) {
    if (m_readiness.load(std::memory_order_acquire) & _::kWritable) {
      return true;
    }
    set_waker(m_write_waker_sched, m_write_waker_task, waker);
    if (m_readiness.load(std::memory_order_acquire) & _::kWritable) {
      clear_waker(m_write_waker_sched, m_write_waker_task);
      return true;
    }
    return false;
  }

  /** @brief Clear the readable bit after recv() returns EAGAIN. */
  void clear_readable() {
    m_readiness.fetch_and(~_::kReadable, std::memory_order_acq_rel);
  }

  /** @brief Clear the writable bit after send() returns EAGAIN. */
  void clear_writable() {
    m_readiness.fetch_and(~_::kWritable, std::memory_order_acq_rel);
  }

  /**
   * @brief Close the FD and mark it as closed.
   * Wakes both directions so pending reads/writes observe EOF.
   */
  void close_fd() {
    uint8_t prev = m_readiness.fetch_or(_::kClosed | _::kReadable | _::kWritable,
                                         std::memory_order_acq_rel);
    if (!(prev & _::kClosed)) {
      if (m_source) {
        xEventDel(m_loop, m_source);
        m_source = nullptr;
      }
      if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
      }
      wake_read();
      wake_write();
    }
  }

  bool is_closed() const {
    return m_readiness.load(std::memory_order_acquire) & _::kClosed;
  }

private:
  /* ── Waker slot helpers (atomic pair: Schedule* + SpawnTaskBase*) ── */

  static void set_waker(std::atomic<_::Schedule *> &sched_slot,
                         std::atomic<_::SpawnTaskBase *> &task_slot,
                         _::Waker waker) {
    task_slot.store(waker.task(), std::memory_order_relaxed);
    sched_slot.store(waker.sched(), std::memory_order_release);
  }

  static void clear_waker(std::atomic<_::Schedule *> &sched_slot,
                            std::atomic<_::SpawnTaskBase *> &task_slot) {
    sched_slot.store(nullptr, std::memory_order_relaxed);
    task_slot.store(nullptr, std::memory_order_relaxed);
  }

  static void fire_waker(std::atomic<_::Schedule *> &sched_slot,
                           std::atomic<_::SpawnTaskBase *> &task_slot) {
    _::Schedule *s = sched_slot.exchange(nullptr, std::memory_order_acq_rel);
    _::SpawnTaskBase *t = task_slot.exchange(nullptr, std::memory_order_relaxed);
    if (s) s->schedule(t);
  }

  void wake_read() { fire_waker(m_read_waker_sched, m_read_waker_task); }
  void wake_write() { fire_waker(m_write_waker_sched, m_write_waker_task); }

  /* ── Event callback ─────────────────────────────────────────────── */

  static void on_event(int /*fd*/, xEventMask mask, void *arg) {
    auto *self = static_cast<ScheduledIo *>(arg);
    if (mask & xEvent_Read) {
      self->m_readiness.fetch_or(_::kReadable, std::memory_order_acq_rel);
      self->wake_read();
    }
    if (mask & xEvent_Write) {
      self->m_readiness.fetch_or(_::kWritable, std::memory_order_acq_rel);
      self->wake_write();
    }
  }

  /* ── Fields ─────────────────────────────────────────────────────── */

  xEventLoop                          m_loop;
  int                                 m_fd;
  xEventSource                        m_source{nullptr};
  std::atomic<uint8_t>                m_readiness;

  // Read waker slot (atomic pair)
  std::atomic<_::Schedule *>          m_read_waker_sched{nullptr};
  std::atomic<_::SpawnTaskBase *>     m_read_waker_task{nullptr};

  // Write waker slot (atomic pair)
  std::atomic<_::Schedule *>          m_write_waker_sched{nullptr};
  std::atomic<_::SpawnTaskBase *>     m_write_waker_task{nullptr};
};

/* ── Readiness PromiseNode ─────────────────────────────────────────── */

namespace _ {

/**
 * @brief PromiseNode that resolves when a direction becomes ready.
 *
 * Unified for read/write via a function pointer to poll_read or
 * poll_write on the ScheduledIo.
 */
class ReadinessPromiseNode final : public PromiseNode<void> {
public:
  using PollFn = bool (ScheduledIo::*)(Waker);

  ReadinessPromiseNode(Arc<ScheduledIo> sio, PollFn fn)
      : m_sio(std::move(sio)), m_poll_fn(fn) {}

  bool poll(Waker waker) override {
    return (m_sio.get()->*m_poll_fn)(waker);
  }

  Void take() override { return Void{}; }

private:
  Arc<ScheduledIo> m_sio;
  PollFn           m_poll_fn;
};

inline Promise<void> readable(Arc<ScheduledIo> sio) {
  return Promise<void>(Own<PromiseNode<void>>(
    new ReadinessPromiseNode(std::move(sio), &ScheduledIo::poll_read)));
}

inline Promise<void> writable(Arc<ScheduledIo> sio) {
  return Promise<void>(Own<PromiseNode<void>>(
    new ReadinessPromiseNode(std::move(sio), &ScheduledIo::poll_write)));
}

} // namespace _

} // namespace xpp

#endif // XPP_SCHEDULED_IO_H
