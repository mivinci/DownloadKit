/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex.h - Mutex<T>: cooperative async lock for async tasks.
 *
 * Unlike xpp/sys/mutex.h (which blocks the OS thread), this Mutex
 * yields the current task when contended. Waiters are queued and
 * resumed in FIFO order when the lock is released.
 *
 * Usage:
 *   xpp::Mutex<int> counter(0);
 *
 *   Promise<void> increment(Mutex<int> &m) {
 *     auto guard = co_await m.lock();
 *     *guard += 1;
 *   }
 *
 * MutexGuard<T> provides exclusive access to T and releases the lock
 * on destruction. Dropping the guard while other tasks are waiting
 * wakes the next waiter.
 *
 * Thread-safe: multiple tasks on different workers can co_await the
 * same Mutex. The internal queue is protected by a lightweight
 * spinlock (no OS syscall in the uncontended path).
 */

#ifndef XPP_MUTEX_H
#define XPP_MUTEX_H

#include <xpp/promise.h>

#include <atomic>

namespace xpp {

template <class T> class MutexGuard;

/* ── Mutex<T> ────────────────────────────────────────────────────── */

/**
 * @brief Async mutex that protects data T with cooperative locking.
 *
 * lock() returns a Promise<MutexGuard<T>>. If the mutex is free,
 * the promise resolves immediately. If contended, the caller is
 * suspended (co_await) until the lock becomes available.
 *
 * @tparam T  The protected data type.
 */
template <class T> class Mutex {
public:
  explicit Mutex(T value)
      : m_data(std::move(value)), m_locked(false), m_head(nullptr), m_tail(nullptr) {}

  Mutex(const Mutex &)            = delete;
  Mutex &operator=(const Mutex &) = delete;

  /**
   * @brief Acquire the lock asynchronously.
   *
   * Returns a Promise that resolves to a MutexGuard<T> once the lock
   * is acquired. If uncontended, resolves immediately.
   *
   * @code
   *   auto guard = co_await mutex.lock();
   *   *guard = 42;
   * @endcode
   */
  Promise<MutexGuard<T>> lock();

  /**
   * @brief Try to acquire immediately without waiting.
   * @return A guard if successful, empty Option if contended.
   */
  Option<MutexGuard<T>> try_lock();

private:
  friend class MutexGuard<T>;

  void unlock();

  void spin_lock() {
    while (m_spin.exchange(true, std::memory_order_acquire)) {}
  }
  void spin_unlock() {
    m_spin.store(false, std::memory_order_release);
  }

  T                 m_data;
  std::atomic<bool> m_spin{false};
  bool              m_locked;

  struct Waiter {
    _::AdapterPromiseNode<MutexGuard<T>> *adapter;
    Waiter                               *next;
  };
  Waiter *m_head;
  Waiter *m_tail;
};

/* ── MutexGuard<T> ───────────────────────────────────────────────── */

/**
 * @brief RAII guard providing exclusive access to Mutex-protected data.
 *
 * Dereference (*guard or guard->) to access the data. The lock is
 * released when the guard is destroyed or moved-from.
 */
template <class T> class MutexGuard {
public:
  MutexGuard() : m_mutex(nullptr) {}
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

  ~MutexGuard() {
    release();
  }

  T &operator*() {
    return m_mutex->m_data;
  }
  const T &operator*() const {
    return m_mutex->m_data;
  }
  T *operator->() {
    return &m_mutex->m_data;
  }
  const T *operator->() const {
    return &m_mutex->m_data;
  }

private:
  friend class Mutex<T>;
  explicit MutexGuard(Mutex<T> *m) : m_mutex(m) {}

  void release() {
    if (m_mutex) {
      m_mutex->unlock();
      m_mutex = nullptr;
    }
  }

  Mutex<T> *m_mutex;
};

/* ── Mutex<T> implementations ────────────────────────────────────── */

template <class T> Promise<MutexGuard<T>> Mutex<T>::lock() {
  spin_lock();

  if (!m_locked) {
    m_locked = true;
    spin_unlock();
    return Promise<MutexGuard<T>>::resolve(MutexGuard<T>(this));
  }

  auto                              *adapter = new _::AdapterPromiseNode<MutexGuard<T>>();
  Own<_::PromiseNode<MutexGuard<T>>> node{adapter};
  Promise<MutexGuard<T>>             promise{std::move(node)};

  auto *waiter = new Waiter{adapter, nullptr};
  if (m_tail) {
    m_tail->next = waiter;
  } else {
    m_head = waiter;
  }
  m_tail = waiter;

  spin_unlock();
  return promise;
}

template <class T> Option<MutexGuard<T>> Mutex<T>::try_lock() {
  spin_lock();
  if (!m_locked) {
    m_locked = true;
    spin_unlock();
    return MutexGuard<T>(this);
  }
  spin_unlock();
  return none;
}

template <class T> void Mutex<T>::unlock() {
  spin_lock();

  if (m_head) {
    Waiter *w = m_head;
    m_head    = w->next;
    if (!m_head) m_tail = nullptr;
    spin_unlock();

    w->adapter->resolve(MutexGuard<T>(this));
    delete w;
  } else {
    m_locked = false;
    spin_unlock();
  }
}

} // namespace xpp

#endif // XPP_MUTEX_H
