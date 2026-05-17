/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * rc.h - Rc<T>: a non-null, shared-owning, reference-counted handle
 *        to a heap-allocated T. Plus a niche-optimized Option<Rc<T>>
 *        at sizeof(T*).
 *
 *   sizeof(Rc<T>)         == sizeof(T*)         (a single pointer to RcInner)
 *   sizeof(Option<Rc<T>>) == sizeof(T*)         (nullptr ↔ None)
 *
 * Design analogue: Rust's std::rc::Rc<T> + Option<Rc<T>>. Like Rust's
 * Rc, libx++'s Rc is **co-located but NOT intrusive**: T does not
 * need to inherit from anything. make_rc<T>(args...) is the only
 * construction entry — the inner block carries the strong and weak
 * counts next to T, so the whole thing is a single heap allocation.
 *
 * The name "Rc" is deliberate. C++ already has "Ref" overloaded
 * across libraries (WebKit's WTF::Ref is intrusive; Qt's QRef is yet
 * another flavour); the Rc spelling makes it clear which design this
 * one follows, even to readers who haven't read the header.
 *
 * Choose Rc<T> when:
 *   - Ownership is shared (the same T is reachable from several places).
 *   - You want sizeof == sizeof(T*) per handle, not the 2-word
 *     std::shared_ptr layout.
 *   - Single-thread. Cross-thread sharing is undefined behaviour;
 *     use Arc<T> (see xpp/arc.h) for that.
 *
 * Choose Own<T> instead when ownership is unique.
 * Choose NonNull<T> / Option<NonNull<T>> for non-owning references.
 *
 * Cycle breaking:
 *   Reference cycles built out of Rcs alone will leak. Break them
 *   with Weak<T> on the back-edge (see xpp/weak.h). Rust does the
 *   same trick.
 *
 * Reference-count layout (matches Rust):
 *
 *   strong = number of Rc<T>      pointing at this inner
 *   weak   = number of Weak<T>    pointing at this inner, **plus one**
 *            for the set of all live Rc<T>s. That extra +1 means we
 *            never need to special-case "any strong alive?" — when
 *            the last Rc drops it decrements weak normally, and the
 *            inner is deallocated iff weak hits zero. T's destructor
 *            runs at the strong→0 transition; the inner's memory is
 *            freed at the weak→0 transition. Those two events can be
 *            the same call when no Weak ever existed.
 *
 * Thread safety:
 *   Rc<T> is **single-thread**. Both counts are plain size_t and
 *   sharing a single Rc<T> (or its Weak<T>) across threads is UB.
 *   For thread-safe shared ownership use Arc<T> in xpp/arc.h.
 *
 * C++11-compatible. Header-only. Trivially nothing — Rc carries
 * non-trivial copy/destroy semantics that ref-count the inner.
 */

#ifndef XPP_RC_H
#define XPP_RC_H

#include <xpp/option.h>
#include <xpp/panic.h>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace xpp {

template <class T> class Rc;
template <class T> class Weak;

namespace _ {

/**
 * @brief Heap-allocated control block + value, the storage Rc<T>
 *        and Weak<T> share.
 *
 * Layout: { strong, weak, value }. The value is co-located so deref()
 * folds to a constant-offset load on the Rc side, and so the count
 * cache lines are warm whenever the value is.
 *
 * Lifecycle:
 *   - make_rc<T>(...) allocates the inner with strong=1, weak=1.
 *   - Each new Rc<T> bumps strong; each new Weak<T> bumps weak.
 *   - The last Rc to drop destroys T in place (~value), then
 *     decrements weak as if it were a Weak (the "all-strongs-count-
 *     as-one-weak" trick).
 *   - The last Weak to drop deallocates the inner block.
 *
 * `strong` and `weak` are plain size_t — Rc is single-thread. Arc
 * has its own ArcInner with std::atomic<size_t> in xpp/arc.h.
 */
template <class T> struct RcInner {
  size_t strong;
  size_t weak;
  T      value;

  template <class... Args>
  explicit RcInner(Args &&...args) : strong(1), weak(1), value(std::forward<Args>(args)...) {}
};

/**
 * @brief Drop the +1 on the "all strongs" weak side and possibly
 *        deallocate the inner block.
 *
 * Called twice in the lifecycle: once when a Weak drops, once when
 * the last Rc drops (after destroying T). Factored out so both call
 * sites use exactly the same end-of-life decision.
 */
template <class T> inline void rc_dec_weak_and_maybe_dealloc(RcInner<T> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: rc_dec_weak called with null inner");
  if (--inner->weak == 0) ::operator delete(inner);
}

/**
 * @brief Drop a strong reference. Destroy T in place when the last
 *        strong leaves, then decrement weak (the +1 for "any strong
 *        alive"). The inner block itself dies at weak->0, possibly
 *        in a different call if a Weak is still observing.
 */
template <class T> inline void rc_dec_strong(RcInner<T> *inner) noexcept {
  XPP_DEBUG_ASSERT(inner != nullptr, "internal: rc_dec_strong called with null inner");
  if (--inner->strong == 0) {
    inner->value.~T();
    rc_dec_weak_and_maybe_dealloc(inner);
  }
}

} // namespace _

/**
 * @brief Non-null shared-owning reference-counted handle to a
 *        heap-allocated T.
 *
 * Always points at a live T whose RcInner has strong >= 1. Move-from
 * leaves the source in an unspecified state (internally null) —
 * using the moved-from Rc is undefined; only destruction is safe.
 * Same contract as std::unique_ptr's moved-from state.
 *
 * Use make_rc<T>(args...) to construct. There is no public
 * constructor from a raw T* — Rc always owns the allocation it was
 * born with, and a stray pointer can't be retro-fitted with a count.
 */
template <class T> class Rc {
public:
  using value_type = T;

  /**
   * @brief Copy constructor: +1 on the shared strong count.
   *
   * Implicit on purpose. For hot-path code where "this is a new
   * owner" should be visually obvious, call `.clone()` or the
   * Rust-style `Rc<T>::clone(&r)` static helper instead — both do
   * the same thing as copy construction but read as deliberate.
   */
  Rc(const Rc &o) noexcept : m_inner(o.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->strong;
  }

  /**
   * @brief Move constructor: zero count changes; the source becomes
   *        invalid (UB to use other than to destroy).
   */
  Rc(Rc &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  /**
   * @brief Covariant copy: Rc<Derived> → Rc<Base>.
   */
  template <class U, typename = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Rc(const Rc<U> &o) noexcept : m_inner(reinterpret_cast<_::RcInner<T> *>(o.inner_raw())) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->strong;
  }

  /**
   * @brief Covariant move: Rc<Derived> → Rc<Base>.
   */
  template <class U, typename = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Rc(Rc<U> &&o) noexcept : m_inner(reinterpret_cast<_::RcInner<T> *>(o.inner_raw())) {
    o.inner_raw_reset();
  }

  Rc &operator=(const Rc &o) noexcept {
    if (this != &o) {
      Rc tmp(o);
      swap(tmp);
    }
    return *this;
  }

  Rc &operator=(Rc &&o) noexcept {
    if (this != &o) {
      Rc tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~Rc() noexcept {
    if (m_inner) _::rc_dec_strong(m_inner);
  }

  /**
   * @brief Explicit +1, member-style. See also the static
   *        Rc<T>::clone(&r) for the Rust calling convention.
   */
  Rc clone() const noexcept {
    return Rc(*this);
  }

  /**
   * @brief Strong reference count (debugging only).
   *
   * Don't branch on this in production code — a same-thread clone
   * can change it between the read and any decision you make. Mostly
   * useful for assertions and tests ("did this drop?").
   */
  size_t strong_count() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return m_inner->strong;
  }

  /**
   * @brief Weak reference count (debugging only).
   *
   * Counts the live Weak<T>s observing this inner. The "all strongs
   * count as one weak" +1 is NOT included — when only Rcs exist this
   * reads 0, matching Rust's `Rc::weak_count`.
   */
  size_t weak_count() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return m_inner->weak - 1;
  }

  T &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return m_inner->value;
  }
  T *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return &m_inner->value;
  }

  T *get() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    return &m_inner->value;
  }

  void swap(Rc &o) noexcept {
    _::RcInner<T> *tmp = m_inner;
    m_inner            = o.m_inner;
    o.m_inner          = tmp;
  }

  bool operator==(const Rc &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const Rc &o) const noexcept {
    return m_inner != o.m_inner;
  }

  /**
   * @brief Rust-style explicit +1: `Rc<T>::clone(&r)`.
   *
   * Static method that takes a `const Rc<T>&` and returns a new
   * Rc<T>. Same semantics as the member `.clone()` and copy
   * construction.
   */
  static Rc clone(const Rc &r) noexcept {
    return r.clone();
  }

  /**
   * @brief Drop a strong reference, return a non-owning Weak.
   *
   * Same as `Weak<T>(r)`. Rust spells this `Rc::downgrade(&r)`. The
   * caller still owns the original strong reference; the new Weak
   * adds +1 to the weak count.
   */
  static Weak<T> downgrade(const Rc &r) noexcept;

  /* ── internals exposed only to friends / templates ───────────────── */

  // Used by covariant ctor of Rc<Other>; do not call directly.
  _::RcInner<T> *inner_raw() const noexcept {
    return m_inner;
  }
  void inner_raw_reset() noexcept {
    m_inner = nullptr;
  }

private:
  // Constructed only by make_rc, the friend Option specialization,
  // and Weak<T>::upgrade. m_inner is always non-null at this point.
  explicit Rc(_::RcInner<T> *inner) noexcept : m_inner(inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
  }

  _::RcInner<T> *m_inner;

  template <class U, class... Args> friend Rc<U> make_rc(Args &&...args);
  template <class U> friend class Rc;
  template <class U> friend class Weak;
  friend class Option<Rc<T>>;
};

/**
 * @brief Construct an Rc<T> in place.
 *
 * Single heap allocation: the inner block holds the strong count
 * (=1), the weak count (=1, the implicit "all strongs are one weak"
 * marker), and a freshly-constructed T.
 *
 * libx++ does not use exceptions for control flow (see README), so
 * make_rc expects T's constructor not to throw. If T can fail to
 * construct, model that with a static factory returning
 * Result<T, Error> and call make_rc on the unwrapped success path.
 *
 * The only public construction entry point. Rc has no constructor
 * from a raw T*.
 */
template <class T, class... Args> Rc<T> make_rc(Args &&...args) {
  void          *mem   = ::operator new(sizeof(_::RcInner<T>));
  _::RcInner<T> *inner = ::new (mem) _::RcInner<T>(std::forward<Args>(args)...);
  return Rc<T>(inner);
}

template <class T> void swap(Rc<T> &a, Rc<T> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Niche-optimized Option<Rc<T>>. Storage is a single
 *        RcInner<T>* whose nullptr value means None.
 *
 *   sizeof(Option<Rc<T>>) == sizeof(T*)
 *
 * Owns a +1 on the strong count whenever it is Some, releases it on
 * destruction or when overwritten with None.
 */
template <class T> class Option<Rc<T>> {
public:
  using value_type = Rc<T>;

  constexpr Option() noexcept : m_inner(nullptr) {}
  constexpr Option(None) noexcept : m_inner(nullptr) {}

  Option(const Rc<T> &r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->strong;
  }
  Option(Rc<T> &&r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    r.m_inner = nullptr;
  }

  Option(const Option &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) ++m_inner->strong;
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
    if (m_inner) _::rc_dec_strong(m_inner);
  }

  bool is_some() const noexcept {
    return m_inner != nullptr;
  }
  bool is_none() const noexcept {
    return m_inner == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_inner != nullptr;
  }

  Rc<T> unwrap() && {
    XPP_ASSERT(m_inner != nullptr, "unwrap() on None Option");
    _::RcInner<T> *taken = m_inner;
    m_inner              = nullptr;
    return Rc<T>(taken);
  }

  Rc<T> unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Option must be Some");
    _::RcInner<T> *taken = m_inner;
    m_inner              = nullptr;
    return Rc<T>(taken);
  }

  Rc<T> take() {
    return std::move(*this).unwrap();
  }

  void swap(Option &o) noexcept {
    _::RcInner<T> *tmp = m_inner;
    m_inner            = o.m_inner;
    o.m_inner          = tmp;
  }

private:
  _::RcInner<T> *m_inner;

  friend class Weak<T>; // for Weak::upgrade() to construct directly
};

template <class T> void swap(Option<Rc<T>> &a, Option<Rc<T>> &b) noexcept {
  a.swap(b);
}

/* ── invariants pinned at compile time ────────────────────────────── */

static_assert(sizeof(Rc<int>) == sizeof(int *), "Rc<T> must be sizeof(T*)");
static_assert(sizeof(Option<Rc<int>>) == sizeof(int *), "Option<Rc<T>> niche optimisation broken");

} // namespace xpp

#endif // XPP_RC_H
