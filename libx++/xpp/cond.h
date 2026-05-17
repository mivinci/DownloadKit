/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cond.h - Condvar: condition variable companion to Mutex<T>.
 *
 * Rust analogue: std::sync::Condvar. One Condvar pairs with one
 * Mutex; wait(guard) atomically releases the lock + sleeps, and on
 * wakeup re-acquires the lock before returning. Same shape as
 * Rust's Condvar::wait — the guard goes in by reference and stays
 * usable after the call.
 *
 *   xpp::Mutex<Queue> q;
 *   xpp::Condvar      not_empty;
 *
 *   // consumer
 *   auto g = q.lock();
 *   while (g->empty()) not_empty.wait(g);
 *   auto item = g->pop();
 *
 *   // producer
 *   {
 *     auto g = q.lock();
 *     g->push(x);
 *   }
 *   not_empty.notify_one();
 *
 * Spurious wakeups are possible — callers should always re-check
 * the predicate they were waiting on under the lock (the
 * `while (!ready) cond.wait(g);` pattern above).
 *
 * Underlying primitive is libx's xCond, but the header doesn't
 * leak that fact: the storage is an opaque, correctly-sized and
 * -aligned byte buffer (xpp::_::cond_storage), and the bridge
 * helpers in xpp::_::cond_xxx are defined out-of-line in
 * cond.cpp. Consumers therefore don't transitively pick up
 * <pthread.h> on POSIX or <windows.h> on Windows.
 *
 * Move-only-by-deletion (the OS-level condvar must not be
 * relocated while waiters are blocked).
 *
 * C++11-compatible. cond.cpp links libx for the actual condvar
 * implementation; the header itself is libx-free.
 */

#ifndef XPP_COND_H
#define XPP_COND_H

#include <xpp/duration.h>
#include <xpp/mutex.h>
#include <xpp/panic.h>

#include <climits>
#include <cstddef>

namespace xpp {

namespace _ {

/* ── Opaque storage for the underlying OS condvar ───────────────────
 *
 * Sized / aligned to fit any platform's xCond (pthread_cond_t on
 * POSIX, CONDITION_VARIABLE on Windows). The exact requirement is
 * checked in cond.cpp via static_assert against the real type.
 *
 * Why 64 / 16:
 *   pthread_cond_t            macOS:48  glibc-x64:48  glibc-arm64:48
 *                             musl:48   FreeBSD:8 (ptr)
 *   CONDITION_VARIABLE (Win)  8
 * Today 48 would suffice everywhere, but we mirror mutex_storage's
 * 64 / 16 on purpose:
 *   - one-cache-line layout dodges false sharing the same way;
 *   - Mutex<T>+Condvar pairs are the common case, and matching
 *     storage keeps their combined footprint predictable;
 *   - if a future libc grows pthread_cond_t past 48 (it has
 *     happened before on glibc), this header's ABI doesn't move.
 * The 16 B/instance overhead vs. a hypothetical 48-byte buffer is
 * negligible at the typical Condvar count (handful per process).
 */
constexpr std::size_t k_cond_storage_size  = 64;
constexpr std::size_t k_cond_storage_align = alignof(void *) * 2;

struct alignas(k_cond_storage_align) cond_storage {
  unsigned char buf[k_cond_storage_size];
};

/* ── Bridge to libx's xCond, defined in cond.cpp ────────────────── */

void cond_init(cond_storage *c) noexcept;
void cond_destroy(cond_storage *c) noexcept;
void cond_signal(cond_storage *c) noexcept;
void cond_broadcast(cond_storage *c) noexcept;
void cond_wait(cond_storage *c, mutex_storage *m) noexcept;
int  cond_timed_wait(cond_storage *c, mutex_storage *m, unsigned timeout_ms) noexcept;

} // namespace _

/**
 * @brief Condition variable companion to Mutex<T>.
 *
 * One Condvar can pair with one Mutex. wait(guard) atomically
 * releases @p guard's lock + sleeps; on wakeup re-acquires the
 * lock before returning. notify_one / notify_all wake one / all
 * waiters; calling them without holding the matching lock is
 * allowed but typically not what you want.
 *
 * Method names mirror Rust's std::sync::Condvar (wait,
 * wait_timeout, notify_one, notify_all), but follow this
 * codebase's small-camel-case convention for public API
 * (wait_timeout, notify_one, notify_all). The wait_timeout return
 * is a plain `bool timed_out` rather than Rust's
 * WaitTimeoutResult, since libx has no poisoning concept.
 */
class Condvar {
public:
  Condvar() noexcept {
    _::cond_init(&m_cond);
  }

  ~Condvar() noexcept {
    _::cond_destroy(&m_cond);
  }

  Condvar(const Condvar &)            = delete;
  Condvar &operator=(const Condvar &) = delete;
  Condvar(Condvar &&)                 = delete;
  Condvar &operator=(Condvar &&)      = delete;

  /**
   * @brief Atomically release @p guard's lock + wait for a signal.
   *
   * On wakeup, re-acquires @p guard's lock before returning. The
   * guard is taken by non-const reference so the caller's
   * MutexGuard remains usable across the call. Same shape as
   * Rust's Condvar::wait.
   *
   * Spurious wakeups can occur: callers should re-check the
   * predicate they were waiting on, e.g.
   *
   *   while (!ready) cv.wait(g);
   */
  template <class T> void wait(MutexGuard<T> &guard) noexcept {
    XPP_DEBUG_ASSERT(guard.m_mutex != nullptr,
                     "internal: Condvar::wait on moved-from MutexGuard");
    _::cond_wait(&m_cond, &guard.m_mutex->m_mutex);
  }

  /**
   * @brief Like wait, but with a timeout.
   *
   * Mirrors Rust's Condvar::wait_timeout. The lock is re-acquired
   * on every return path, so @p guard remains usable after the
   * call regardless of whether we were signalled or timed out.
   *
   * Spurious wakeups still apply: a `false` return means the
   * deadline did not elapse, but does not promise the caller's
   * predicate is satisfied. Re-check it.
   *
   * @param timeout  Maximum time to wait. Saturates at ~49 days
   *                 (UINT_MAX milliseconds), since the underlying
   *                 libx call takes a 32-bit ms count; longer
   *                 timeouts are clamped without panicking.
   * @return         True if the wait timed out, false if we were
   *                 woken up before the deadline. Same shape as
   *                 Rust's WaitTimeoutResult::timed_out().
   */
  template <class T> bool wait_timeout(MutexGuard<T> &guard, Duration timeout) noexcept {
    XPP_DEBUG_ASSERT(guard.m_mutex != nullptr,
                     "internal: Condvar::wait_timeout on moved-from MutexGuard");
    const uint64_t ms_u64 = timeout.as_millis();
    const unsigned ms     = ms_u64 > UINT_MAX ? UINT_MAX : static_cast<unsigned>(ms_u64);
    return _::cond_timed_wait(&m_cond, &guard.m_mutex->m_mutex, ms) != 0;
  }

  /** @brief Wake one waiter (or no-op if none). Mirrors Rust's notify_one. */
  void notify_one() noexcept {
    _::cond_signal(&m_cond);
  }

  /** @brief Wake every waiter. Mirrors Rust's notify_all. */
  void notify_all() noexcept {
    _::cond_broadcast(&m_cond);
  }

private:
  _::cond_storage m_cond;
};

} // namespace xpp

#endif // XPP_COND_H
