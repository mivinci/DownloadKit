/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arc.h - Arc<T>: the atomic, thread-safe counterpart of Rc<T>. Plus
 *         ArcWeak<T> and a niche-optimized Option<Arc<T>> at
 *         sizeof(T*).
 *
 *   sizeof(Arc<T>)         == sizeof(T*)
 *   sizeof(Option<Arc<T>>) == sizeof(T*)
 *   sizeof(ArcWeak<T>)     == sizeof(T*)
 *
 * Design analogue: Rust's std::sync::Arc<T> + Weak<T>. Same shape
 * as xpp::Rc<T> / xpp::Weak<T>, except the strong and weak counts
 * are std::atomic<size_t> and the operations use the carefully
 * chosen memory order pattern documented below.
 *
 * Rc vs Arc:
 *   - Rc<T> (xpp/rc.h): single-thread. clone/drop are plain ++/--.
 *   - Arc<T> (this file): cross-thread. clone/drop use atomic
 *     fetch_add / fetch_sub. About 3-5× slower than Rc clone on
 *     contended hardware, free on uncontended cache.
 *
 * Pick Rc for graphs that never cross a thread boundary; Arc only
 * when sharing across threads is real. Mixing the two for the same
 * object isn't supported (and isn't needed — one or the other
 * suffices for any given lifetime).
 *
 * Memory order discipline (matches Rust libstd / triomphe / boost):
 *
 *   strong clone (fetch_add(1)):   memory_order_relaxed
 *     The new owner does not synchronise with anything by virtue of
 *     incrementing; only side-effects through the value need
 *     ordering, and that's the caller's responsibility.
 *
 *   strong drop (fetch_sub(1)):    memory_order_release
 *     The dropping thread "releases" all writes it made to the
 *     pointee, so a thread that eventually destroys T can observe
 *     them after acquiring.
 *
 *   When strong hits 0:            atomic_thread_fence(acquire)
 *     Pair with the release above so the destruction sees writes
 *     from every previous owner. Cheaper than acq_rel on every
 *     drop — only the destroying thread pays.
 *
 *   weak clone (fetch_add(1)):     memory_order_relaxed
 *     Same reasoning as strong clone.
 *
 *   weak drop (fetch_sub(1)):      memory_order_release
 *     Pair with the acquire fence in the deallocator.
 *
 *   When weak hits 0:              atomic_thread_fence(acquire)
 *     Same pattern as for strong; the freeing thread acquires.
 *
 *   ArcWeak::upgrade:              CAS loop on strong
 *     Naïve "if strong > 0, ++strong" races with another thread's
 *     drop. Compare-exchange with relaxed-on-failure handles it.
 *
 * Reference-count layout (same as Rc):
 *
 *   strong = number of Arc<T>     pointing at this inner
 *   weak   = number of ArcWeak<T> + 1 for the set of all live Arcs
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_ARC_H
#define XPP_ARC_H

#include <xpp/option.h>
#include <xpp/panic.h>

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace xpp {

template <class T> class Arc;
template <class T> class ArcWeak;

namespace _ {

/**
 * @brief Atomic counterpart of RcInner. Same layout strategy, but
 *        strong/weak are std::atomic<size_t>.
 *
 * The atomic itself is the only thing the threads coordinate on;
 * the value field is accessed only through Arc<T> which guarantees
 * the inner is live for the duration.
 */
template <class T> struct ArcInner {
  std::atomic<size_t> strong;
  std::atomic<size_t> weak;
  T                   value;

  template <class... Args>
  explicit ArcInner(Args &&...args)
      : strong(1), weak(1), value(std::forward<Args>(args)...) {}
};

/**
 * @brief Drop a weak reference and possibly deallocate the inner.
 *
 * `release` on the decrement so the freeing thread (the one that
 * decrements to 0) can `acquire` and see every previous owner's
 * stores to the inner block.
 */
template <class T> inline void arcDecWeakAndMaybeDealloc(ArcInner<T> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: arcDecWeak called with null inner");
  if (inner->weak.fetch_sub(1, std::memory_order_release) == 1) {
    std::atomic_thread_fence(std::memory_order_acquire);
    ::operator delete(inner);
  }
}

/**
 * @brief Drop a strong reference. Same two-stage strategy as the
 *        Rc helper, but with the atomic ordering pattern above.
 */
template <class T> inline void arcDecStrong(ArcInner<T> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: arcDecStrong called with null inner");
  if (inner->strong.fetch_sub(1, std::memory_order_release) == 1) {
    // Last strong gone. Acquire-fence to see every prior owner's
    // writes to *T*, then destroy T in place. The inner's memory
    // sticks around until the weak count also hits zero.
    std::atomic_thread_fence(std::memory_order_acquire);
    inner->value.~T();
    arcDecWeakAndMaybeDealloc(inner);
  }
}

} // namespace _

/**
 * @brief Non-null, thread-safe shared-owning reference-counted
 *        handle to a heap-allocated T.
 *
 * Identical semantics to Rc<T> except the counts are atomic and the
 * type is safe to clone/drop from multiple threads simultaneously.
 * Copy-construct on the consumer thread, drop on whichever thread
 * happens to own the last reference last — the destructor runs
 * exactly once and with a happens-before of every prior owner's
 * stores to T.
 *
 * Use makeArc<T>(args...) to construct.
 */
template <class T> class Arc {
public:
  using value_type = T;

  Arc(const Arc &o) noexcept : m_inner(o.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    // relaxed: a fresh owner does not synchronise with anything by
    // the fact of incrementing.
    m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }

  Arc(Arc &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  template <class U, typename = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Arc(const Arc<U> &o) noexcept : m_inner(reinterpret_cast<_::ArcInner<T> *>(o.innerRaw())) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }

  template <class U, typename = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Arc(Arc<U> &&o) noexcept : m_inner(reinterpret_cast<_::ArcInner<T> *>(o.innerRaw())) {
    o.innerRawReset();
  }

  Arc &operator=(const Arc &o) noexcept {
    if (this != &o) {
      Arc tmp(o);
      swap(tmp);
    }
    return *this;
  }

  Arc &operator=(Arc &&o) noexcept {
    if (this != &o) {
      Arc tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~Arc() noexcept {
    if (m_inner) _::arcDecStrong(m_inner);
  }

  Arc clone() const noexcept {
    return Arc(*this);
  }

  /**
   * @brief Strong count (debugging / coarse instrumentation).
   *
   * Loads with relaxed — by the time you read it another thread may
   * have changed it, so don't branch on the result.
   */
  size_t strongCount() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return m_inner->strong.load(std::memory_order_relaxed);
  }

  /**
   * @brief Weak count (debugging only). Subtracts the +1 for "all
   *        strongs", matching Rust's Arc::weak_count.
   */
  size_t weakCount() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    const size_t w = m_inner->weak.load(std::memory_order_relaxed);
    return w - 1;
  }

  T &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return m_inner->value;
  }
  T *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return &m_inner->value;
  }
  T *get() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    return &m_inner->value;
  }

  void swap(Arc &o) noexcept {
    _::ArcInner<T> *tmp = m_inner;
    m_inner             = o.m_inner;
    o.m_inner           = tmp;
  }

  bool operator==(const Arc &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const Arc &o) const noexcept {
    return m_inner != o.m_inner;
  }

  static Arc clone(const Arc &r) noexcept {
    return r.clone();
  }

  static ArcWeak<T> downgrade(const Arc &r) noexcept;

  /* ── internals exposed only to friends / templates ───────────────── */

  _::ArcInner<T> *innerRaw() const noexcept {
    return m_inner;
  }
  void innerRawReset() noexcept {
    m_inner = nullptr;
  }

private:
  explicit Arc(_::ArcInner<T> *inner) noexcept : m_inner(inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
  }

  _::ArcInner<T> *m_inner;

  template <class U, class... Args> friend Arc<U> makeArc(Args &&...args);
  template <class U> friend class Arc;
  template <class U> friend class ArcWeak;
  friend class Option<Arc<T>>;
};

/**
 * @brief Construct an Arc<T> in place.
 *
 * Single heap allocation containing the strong count (=1), the
 * weak count (=1, the "all strongs as one weak" marker), and T
 * constructed from @p args.
 */
template <class T, class... Args> Arc<T> makeArc(Args &&...args) {
  void           *mem   = ::operator new(sizeof(_::ArcInner<T>));
  _::ArcInner<T> *inner = ::new (mem) _::ArcInner<T>(std::forward<Args>(args)...);
  return Arc<T>(inner);
}

template <class T> void swap(Arc<T> &a, Arc<T> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Niche-optimized Option<Arc<T>>. Storage is a single
 *        ArcInner<T>* whose nullptr value means None.
 */
template <class T> class Option<Arc<T>> {
public:
  using value_type = Arc<T>;

  constexpr Option() noexcept : m_inner(nullptr) {}
  constexpr Option(None) noexcept : m_inner(nullptr) {}

  Option(const Arc<T> &r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }
  Option(Arc<T> &&r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    r.m_inner = nullptr;
  }

  Option(const Option &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) m_inner->strong.fetch_add(1, std::memory_order_relaxed);
  }
  Option(Option &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  Option &operator=(const Option &o) noexcept {
    if (this != &o) {
      Option tmp(o);
      swap(tmp);
    }
    return *this;
  }
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      Option tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }
  Option &operator=(None) noexcept {
    Option tmp;
    swap(tmp);
    return *this;
  }

  ~Option() noexcept {
    if (m_inner) _::arcDecStrong(m_inner);
  }

  bool isSome() const noexcept {
    return m_inner != nullptr;
  }
  bool isNone() const noexcept {
    return m_inner == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_inner != nullptr;
  }

  Arc<T> unwrap() && {
    XPP_ASSERT(m_inner != nullptr, "unwrap() on None Option");
    _::ArcInner<T> *taken = m_inner;
    m_inner               = nullptr;
    return Arc<T>(taken);
  }

  Arc<T> unwrapUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Option must be Some");
    _::ArcInner<T> *taken = m_inner;
    m_inner               = nullptr;
    return Arc<T>(taken);
  }

  Arc<T> take() {
    return std::move(*this).unwrap();
  }

  void swap(Option &o) noexcept {
    _::ArcInner<T> *tmp = m_inner;
    m_inner             = o.m_inner;
    o.m_inner           = tmp;
  }

private:
  _::ArcInner<T> *m_inner;

  friend class ArcWeak<T>; // for ArcWeak::upgrade to build a Some directly
};

template <class T> void swap(Option<Arc<T>> &a, Option<Arc<T>> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Non-owning, thread-safe observer of an Arc<T>'s inner.
 *
 * Default-constructs to null. Constructing from an Arc bumps the
 * inner's weak count atomically. `upgrade()` does a CAS loop on the
 * strong count so it sees a consistent "is anyone still strong?"
 * snapshot even if another thread is dropping the last Arc.
 */
template <class T> class ArcWeak {
public:
  constexpr ArcWeak() noexcept : m_inner(nullptr) {}

  explicit ArcWeak(const Arc<T> &r) noexcept : m_inner(r.innerRaw()) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Arc must own an inner");
    m_inner->weak.fetch_add(1, std::memory_order_relaxed);
  }

  ArcWeak(const ArcWeak &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) m_inner->weak.fetch_add(1, std::memory_order_relaxed);
  }
  ArcWeak(ArcWeak &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  ArcWeak &operator=(const ArcWeak &o) noexcept {
    if (this != &o) {
      ArcWeak tmp(o);
      swap(tmp);
    }
    return *this;
  }
  ArcWeak &operator=(ArcWeak &&o) noexcept {
    if (this != &o) {
      ArcWeak tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~ArcWeak() noexcept {
    if (m_inner) _::arcDecWeakAndMaybeDealloc(m_inner);
  }

  /**
   * @brief Attempt to obtain a strong Arc.
   *
   * CAS loop: read strong, fail if 0, else compare-exchange to
   * strong+1. The compare-exchange is `acquire` on success so the
   * resulting Arc synchronises-with prior strong drops (we need to
   * see all the writes that landed before the last drop, in case
   * another thread happens to be in the middle of dropping); the
   * failure load can be relaxed.
   *
   * Returns None if every strong has been dropped (T destroyed) or
   * this ArcWeak is null.
   */
  Option<Arc<T>> upgrade() const noexcept {
    if (!m_inner) return Option<Arc<T>>();
    size_t s = m_inner->strong.load(std::memory_order_relaxed);
    for (;;) {
      if (s == 0) return Option<Arc<T>>();
      if (m_inner->strong.compare_exchange_weak(
              s, s + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        Option<Arc<T>> out;
        out.m_inner = m_inner;
        return out;
      }
      // s was updated to the current value by the failed exchange;
      // loop and retry.
    }
  }

  size_t strongCount() const noexcept {
    return m_inner ? m_inner->strong.load(std::memory_order_relaxed) : 0;
  }

  size_t weakCount() const noexcept {
    if (!m_inner) return 0;
    const size_t s = m_inner->strong.load(std::memory_order_relaxed);
    const size_t w = m_inner->weak.load(std::memory_order_relaxed);
    return s > 0 ? w - 1 : w;
  }

  bool isExpired() const noexcept {
    return !m_inner || m_inner->strong.load(std::memory_order_relaxed) == 0;
  }

  void swap(ArcWeak &o) noexcept {
    _::ArcInner<T> *tmp = m_inner;
    m_inner             = o.m_inner;
    o.m_inner           = tmp;
  }

  bool operator==(const ArcWeak &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const ArcWeak &o) const noexcept {
    return m_inner != o.m_inner;
  }

private:
  _::ArcInner<T> *m_inner;
};

template <class T> void swap(ArcWeak<T> &a, ArcWeak<T> &b) noexcept {
  a.swap(b);
}

/* ── deferred member impl: Arc<T>::downgrade ─────────────────────────── */

template <class T> inline ArcWeak<T> Arc<T>::downgrade(const Arc<T> &r) noexcept {
  return ArcWeak<T>(r);
}

/* ── invariants pinned at compile time ───────────────────────────────── */

static_assert(sizeof(Arc<int>) == sizeof(int *), "Arc<T> must be sizeof(T*)");
static_assert(sizeof(Option<Arc<int>>) == sizeof(int *),
              "Option<Arc<T>> niche optimisation broken");
static_assert(sizeof(ArcWeak<int>) == sizeof(int *), "ArcWeak<T> must be sizeof(int*)");

} // namespace xpp

#endif // XPP_ARC_H
