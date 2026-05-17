/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex.h - Mutex<T> + MutexGuard<T>.
 *
 * Rust-style data + lock fusion. Mutex<T> wraps a T with the
 * synchronisation primitive that protects it; the only way to
 * touch the T is through a MutexGuard<T> obtained from Mutex<T>::
 * lock() or try_lock(). The compiler statically prevents the
 * common "I forgot to take the lock" footgun: outside a guard,
 * the data is private.
 *
 *   xpp::Mutex<std::vector<int>> shared;
 *
 *   {
 *     auto g = shared.lock();      // RAII: locked here
 *     g->push_back(42);            // -> proxies to the vector
 *     g->size();
 *   }                              // unlocked here
 *
 * For waiting on a condition, pair with Condvar in xpp/cond.h —
 * Rust splits std::sync::Mutex and std::sync::Condvar into separate
 * types and we follow suit so each header stays focused.
 *
 * Underlying primitive is libx's xMutex, but this header doesn't
 * leak that fact: the lock storage is an opaque, correctly-sized
 * and -aligned byte buffer (xpp::_::mutex_storage), and the bridge
 * helpers in xpp::_::mutex_xxx are defined out-of-line in mutex.cpp.
 * Consumers of <xpp/mutex.h> therefore don't transitively pull in
 * <pthread.h> on POSIX or <windows.h> on Windows, and never see
 * the xMutex / xMutexLock / … C symbols.
 *
 * Thread safety:
 *   Mutex<T> is, of course, designed for cross-thread sharing.
 *   Mutex<T> itself is move-only-by-deletion (the underlying
 *   lock must not be relocated while waiters block on it).
 *   MutexGuard<T> is move-only, never copyable — the lock follows
 *   ownership.
 *
 * C++11-compatible. mutex.cpp links libx for the actual lock
 * implementation; the header itself is libx-free.
 */

#ifndef XPP_MUTEX_H
#define XPP_MUTEX_H

#include <xpp/option.h>
#include <xpp/panic.h>

#include <cstddef>
#include <utility>

namespace xpp {

template <class T> class MutexGuard;
template <class T> class Mutex;
class Condvar;

namespace _ {

/* ── Opaque storage for the underlying OS lock ──────────────────────
 *
 * Sized / aligned to fit any platform's xMutex (pthread_mutex_t on
 * POSIX, CRITICAL_SECTION on Windows). The exact requirement is
 * checked in mutex.cpp via static_assert against the real type, so
 * if a future platform needs more room the build fails loudly
 * instead of silently writing past the buffer.
 *
 * Why 64 / 16:
 *   pthread_mutex_t           macOS:64  glibc-x64:40  glibc-arm64:48
 *                             musl:40   FreeBSD:8 (ptr)
 *   CRITICAL_SECTION (Win)    40
 *   SRWLOCK          (Win)    8
 * macOS sits right on 64; everything else is well under. 16-byte
 * alignment covers the 8-byte alignof every platform reports today
 * with headroom for a future bump to e.g. __m128. Don't shrink
 * without re-running the static_assert on macOS.
 *
 * Padding cost — yes, on non-macOS we waste 16..56 bytes per lock.
 * That's deliberate:
 *   - mutexes are not high-cardinality: a few hundred long-lived
 *     instances per process is typical, so 100 * 24B = 2.4 KB lives
 *     comfortably in the noise;
 *   - 64B happens to be a cache line on every CPU we target
 *     (x86_64 / arm64), so a padded mutex naturally avoids false
 *     sharing with neighbouring hot data — folly / TBB and friends
 *     pad their fast locks to a cache line on purpose;
 *   - per-platform sizes would make sizeof(Mutex<T>) ABI-variant
 *     across OSes and bleed #ifdefs into a header whose whole job
 *     is to hide the OS primitive.
 * If you ever need to pack thousands of locks (e.g. one per
 * connection in a 10k-conn server), reach for a more specialised
 * primitive instead of shrinking this storage.
 */
constexpr std::size_t k_mutex_storage_size  = 64;
constexpr std::size_t k_mutex_storage_align = alignof(void *) * 2;

struct alignas(k_mutex_storage_align) mutex_storage {
  unsigned char buf[k_mutex_storage_size];
};

/* ── Bridge to libx's xMutex, defined in mutex.cpp ──────────────── */

void mutex_init(mutex_storage *s) noexcept;
void mutex_destroy(mutex_storage *s) noexcept;
void mutex_lock(mutex_storage *s) noexcept;
int  mutex_try_lock(mutex_storage *s) noexcept;
void mutex_unlock(mutex_storage *s) noexcept;

} // namespace _

/**
 * @brief RAII guard returned by Mutex<T>::lock / try_lock.
 *
 * Owns the lock for the lifetime of this object: destruction
 * releases it. Move-only — the lock follows ownership. Provides
 * pointer-style access to the protected T.
 */
template <class T> class MutexGuard {
public:
  /** @brief Move ctor: take over the lock from @p o. */
  MutexGuard(MutexGuard &&o) noexcept : m_mutex(o.m_mutex) {
    o.m_mutex = nullptr;
  }

  MutexGuard &operator=(MutexGuard &&o) noexcept {
    if (this != &o) {
      release();
      m_mutex   = o.m_mutex;
      o.m_mutex = nullptr;
    }
    return *this;
  }

  MutexGuard(const MutexGuard &)            = delete;
  MutexGuard &operator=(const MutexGuard &) = delete;

  ~MutexGuard() noexcept {
    release();
  }

  T &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_mutex != nullptr, "internal: MutexGuard moved-from");
    return m_mutex->m_value;
  }
  T *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_mutex != nullptr, "internal: MutexGuard moved-from");
    return &m_mutex->m_value;
  }

  /**
   * @brief Borrow the protected T as a reference.
   *
   * Equivalent to *guard. The pointer / reference is valid as long
   * as this guard is alive.
   */
  T &get() const noexcept {
    XPP_DEBUG_ASSERT(m_mutex != nullptr, "internal: MutexGuard moved-from");
    return m_mutex->m_value;
  }

private:
  // Constructed only by Mutex::lock / try_lock.
  explicit MutexGuard(Mutex<T> *m) noexcept : m_mutex(m) {
    XPP_DEBUG_ASSERT(m != nullptr, "internal: MutexGuard with null Mutex");
  }

  void release() noexcept {
    if (m_mutex) {
      _::mutex_unlock(&m_mutex->m_mutex);
      m_mutex = nullptr;
    }
  }

  Mutex<T> *m_mutex;

  friend class Mutex<T>;
  friend class Condvar; // for wait/wait_timeout access to m_mutex
};

/**
 * @brief Data + lock fusion. The protected T is reachable only
 *        through a MutexGuard returned by lock() / try_lock().
 *
 * Constructed in place from T's ctor arguments:
 *   xpp::Mutex<std::vector<int>> shared;          // empty vector
 *   xpp::Mutex<MyState>          s(arg1, arg2);   // forwarded
 *
 * Move-only-by-deletion: the underlying lock is an OS-level
 * primitive that must not be relocated while threads are waiting
 * on it. Copy is also forbidden (a Mutex protects a *specific* T;
 * copies would have separate locks but appear to share the data).
 */
template <class T> class Mutex {
public:
  using value_type = T;

  /**
   * @brief Construct in place. Forwards @p args to T's ctor.
   *
   * The mutex is initialised on construction and destroyed by ~Mutex.
   */
  template <class... Args> explicit Mutex(Args &&...args) : m_value(std::forward<Args>(args)...) {
    _::mutex_init(&m_mutex);
  }

  ~Mutex() noexcept {
    _::mutex_destroy(&m_mutex);
  }

  Mutex(const Mutex &)            = delete;
  Mutex &operator=(const Mutex &) = delete;
  Mutex(Mutex &&)                 = delete; // OS lock can't be relocated
  Mutex &operator=(Mutex &&)      = delete;

  /**
   * @brief Acquire the lock; blocks until granted.
   *
   * Mirrors Rust's Mutex::lock (minus the LockResult — libx has
   * no poisoning concept). Returns a MutexGuard that owns the
   * lock for its lifetime.
   *
   * Same-thread re-entry is undefined (the underlying lock is NOT
   * recursive on POSIX; on Windows CRITICAL_SECTION it is, but
   * relying on that would be non-portable).
   */
  MutexGuard<T> lock() noexcept {
    _::mutex_lock(&m_mutex);
    return MutexGuard<T>(this);
  }

  /**
   * @brief Try to acquire the lock without blocking.
   *
   * Mirrors Rust's Mutex::try_lock.
   *
   * @return Some(guard) if acquired, None if another thread holds
   *         the lock right now. No retry; for "wait up to N ms",
   *         pair with a Condvar::wait_timeout pattern instead.
   */
  Option<MutexGuard<T>> try_lock() noexcept {
    if (_::mutex_try_lock(&m_mutex) == 0) return Option<MutexGuard<T>>(MutexGuard<T>(this));
    return Option<MutexGuard<T>>();
  }

private:
  _::mutex_storage m_mutex;
  T                m_value;

  friend class MutexGuard<T>;
  friend class Condvar; // for wait/wait_timeout to reach m_mutex
};

} // namespace xpp

#endif // XPP_MUTEX_H
