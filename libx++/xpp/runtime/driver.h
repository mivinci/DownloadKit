/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * driver.h - Event loop as driver + park + timer (libx xEventLoop).
 *
 * ## Divergence from Tokio (intentional, fixed here)
 *
 * Tokio splits the runtime's "wait for work" machinery into two layers:
 *   - runtime/driver.rs : the I/O + time + signal driver (epoll/kqueue
 *     readiness, timer wheel).
 *   - runtime/park.rs   : a thread parking primitive (condvar-based)
 *     used when no I/O driver is present.
 *
 * moo collapses both into libx's xEventLoop, which is simultaneously:
 *   - the I/O readiness driver (paired with xpp::runtime::ScheduledIo),
 *   - the timer source (xEventLoop embeds a timer min-heap; xEventWait
 *     clamps its timeout to the earliest deadline),
 *   - the park / unpark primitive (xEventWait blocks the owning thread;
 *     xEventWake / xEventLoopPost wake it from any thread).
 *
 * Driver below is the thin RAII owner of that loop, exposing the
 * park/unpark vocabulary. The raw handle() is still handed to the
 * readiness layer (ScheduledIo) and to wakers (CoroWakeSchedule /
 * SyncWaitSchedule post their resume callbacks onto it).
 */

#ifndef XPP_RUNTIME_DRIVER_H
#define XPP_RUNTIME_DRIVER_H

#include <xpp/box.h>

#include <utility>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {
namespace runtime {

/* ── EventLoopDeleter ────────────────────────────────────────────── */

/** @brief Box deleter that destroys an owned xEventLoop. */
struct EventLoopDeleter {
  void operator()(void *p) const noexcept {
    xEventLoopDestroy(p);
  }
};

/* ── Unparker ────────────────────────────────────────────────────── */

/**
 * @brief A cheap, copyable handle that can wake a parked Driver from
 *        any thread (mirrors Tokio's park::Unparker).
 *
 * Non-owning: valid only while the underlying Driver is alive.
 */
class Unparker {
public:
  explicit Unparker(xEventLoop loop) noexcept : m_loop(loop) {}

  /** @brief Wake the driver's thread out of park(), if parked. */
  void unpark() const noexcept {
    if (m_loop) xEventWake(m_loop);
  }

  xEventLoop handle() const noexcept {
    return m_loop;
  }

private:
  xEventLoop m_loop;
};

/* ── Driver ──────────────────────────────────────────────────────── */

/**
 * @brief Owns an xEventLoop and exposes park / unpark + the raw handle.
 *
 * One Driver backs each parkable thread: the block_on thread (Runtime)
 * and every multi-thread worker. Move-only (the loop must not be
 * copied); the owning thread parks, any thread may unpark.
 */
class Driver {
public:
  /** @brief Create a Driver owning a fresh event loop. */
  static Driver create() {
    return Driver(Box<void, EventLoopDeleter>::from_raw(xEventLoopCreate()));
  }

  explicit Driver(Box<void, EventLoopDeleter> loop) : m_loop(std::move(loop)) {}

  Driver(Driver &&)            = default;
  Driver &operator=(Driver &&) = default;
  Driver(const Driver &)            = delete;
  Driver &operator=(const Driver &) = delete;

  /** @brief Park the calling thread until woken (blocks indefinitely). */
  void park() noexcept {
    xEventWait(m_loop.get(), -1);
  }
  /** @brief Park up to @p timeout_ms (also fires due timers / I/O). */
  void park_timeout(int timeout_ms) noexcept {
    xEventWait(m_loop.get(), timeout_ms);
  }
  /** @brief Wake the parked thread (thread-safe). */
  void unpark() const noexcept {
    xEventWake(m_loop.get());
  }
  /** @brief A standalone unparker usable from other threads. */
  Unparker unparker() const noexcept {
    return Unparker(m_loop.get());
  }

  /**
   * @brief The raw event loop, for the readiness layer (ScheduledIo)
   *        and for posting wakeups (xEventLoopPost via wakers).
   */
  xEventLoop handle() const noexcept {
    return m_loop.get();
  }

private:
  Box<void, EventLoopDeleter> m_loop;
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_DRIVER_H
