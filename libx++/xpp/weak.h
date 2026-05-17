/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * weak.h - Weak<T>: a non-owning, count-tracking handle to the same
 *          RcInner<T> that backs Rc<T>. Upgrade to Option<Rc<T>> on
 *          demand; the upgrade returns None if every strong has
 *          dropped.
 *
 *   sizeof(Weak<T>) == sizeof(T*)
 *
 * Design analogue: Rust's std::rc::Weak<T>. Like the Rust type,
 * Weak<T> in libx++ is allowed to be null (default constructed
 * yields null — no Option wrapper needed; the "absent" state IS the
 * type's identity). Constructing a Weak from an Rc bumps the inner's
 * weak count; dropping the Weak decrements it. Weak alone never
 * keeps the T alive — only Rc does. The inner's memory stays around
 * until the last Weak goes too, so upgrade() can still safely peek.
 *
 * Use Weak<T> to break reference cycles:
 *
 *   struct Node {
 *     Option<Rc<Node>> next;          // strong forward edge
 *     Weak<Node>       prev;          // weak back-edge — does not
 *                                     // keep the previous node alive
 *   };
 *
 * Reading semantics:
 *   - default ctor / Weak{} → null
 *   - Weak(const Rc<T>&)    → observe; weak += 1
 *   - copy / move           → standard handle moves (copy → weak +=
 *                             1, move → unchanged)
 *   - upgrade() → Option<Rc<T>>: None if strong == 0, else Some(Rc).
 *     The strong is bumped under the existing weak's protection, so
 *     this is sound even if every visible Rc has dropped between
 *     the Weak's construction and the upgrade call.
 *
 * Thread safety: Weak<T> is single-thread. For thread-safe weak refs
 * use ArcWeak<T> in xpp/arc.h.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_WEAK_H
#define XPP_WEAK_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/rc.h>

#include <cstddef>

namespace xpp {

/**
 * @brief Non-owning observer of an Rc<T>'s inner block.
 *
 * May be null. Always safe to call upgrade(); returns None if there
 * are no strong owners left (or if the Weak was default-constructed).
 */
template <class T> class Weak {
public:
  /** @brief Default ctor: null Weak. */
  constexpr Weak() noexcept : m_inner(nullptr) {}

  /**
   * @brief Observe an Rc<T>'s inner.
   *
   * Bumps the inner's weak count by 1. The Rc keeps its strong on
   * the same inner; the Weak does not affect strong.
   */
  explicit Weak(const Rc<T> &r) noexcept : m_inner(r.innerRaw()) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Rc must own an inner");
    ++m_inner->weak;
  }

  Weak(const Weak &o) noexcept : m_inner(o.m_inner) {
    if (m_inner) ++m_inner->weak;
  }
  Weak(Weak &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  Weak &operator=(const Weak &o) noexcept {
    if (this != &o) {
      Weak tmp(o);
      swap(tmp);
    }
    return *this;
  }
  Weak &operator=(Weak &&o) noexcept {
    if (this != &o) {
      Weak tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~Weak() noexcept {
    if (m_inner) _::rcDecWeakAndMaybeDealloc(m_inner);
  }

  /**
   * @brief Attempt to obtain a strong reference.
   *
   * @return Some(Rc<T>) with the strong count bumped, or None if the
   *         underlying T has already been destroyed (strong == 0) or
   *         this Weak is null.
   *
   * The check is the simple `strong != 0` race-free in single-thread
   * land. Across threads use ArcWeak::upgrade in arc.h, which does
   * the CAS dance.
   */
  Option<Rc<T>> upgrade() const noexcept {
    if (!m_inner) return Option<Rc<T>>();
    if (m_inner->strong == 0) return Option<Rc<T>>();
    ++m_inner->strong;
    // Reach into the Option specialization's storage directly. The
    // friend declaration in rc.h makes this legal; we avoid the
    // public `Option(Rc<T>&)` ctor because that would re-bump strong.
    Option<Rc<T>> out;
    out.m_inner = m_inner;
    return out;
  }

  /**
   * @brief Strong count of the observed inner (for tests/debug).
   *
   * Reads 0 if the Weak is null or the underlying T is gone.
   */
  size_t strongCount() const noexcept {
    return m_inner ? m_inner->strong : 0;
  }

  /**
   * @brief Weak count of the observed inner (for tests/debug).
   *
   * Subtracts the implicit "all-strongs-count-as-one-weak" +1 when
   * any strong is alive — same convention as Rc::weakCount() and
   * Rust's Weak::weak_count.
   */
  size_t weakCount() const noexcept {
    if (!m_inner) return 0;
    return m_inner->strong > 0 ? m_inner->weak - 1 : m_inner->weak;
  }

  bool isExpired() const noexcept {
    return !m_inner || m_inner->strong == 0;
  }

  void swap(Weak &o) noexcept {
    _::RcInner<T> *tmp = m_inner;
    m_inner            = o.m_inner;
    o.m_inner          = tmp;
  }

  bool operator==(const Weak &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const Weak &o) const noexcept {
    return m_inner != o.m_inner;
  }

private:
  _::RcInner<T> *m_inner;
};

template <class T> void swap(Weak<T> &a, Weak<T> &b) noexcept {
  a.swap(b);
}

/* ── deferred member impl: Rc<T>::downgrade ──────────────────────────── */

template <class T> inline Weak<T> Rc<T>::downgrade(const Rc<T> &r) noexcept {
  return Weak<T>(r);
}

static_assert(sizeof(Weak<int>) == sizeof(int *), "Weak<T> must be sizeof(T*)");

} // namespace xpp

#endif // XPP_WEAK_H
