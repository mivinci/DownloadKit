/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ref.h - Ref<T>: a non-null, shared-owning, reference-counted handle
 *         to a heap-allocated T. Plus a niche-optimized
 *         Option<Ref<T>> at sizeof(T*).
 *
 *   sizeof(Ref<T>)         == sizeof(T*)         (a single pointer to RefInner)
 *   sizeof(Option<Ref<T>>) == sizeof(T*)         (nullptr ↔ None)
 *
 * Design analogue: Rust's Rc<T> + Option<Rc<T>>. Like Rc, libx++'s Ref
 * is co-located but NOT intrusive: T does not need to inherit from
 * anything. makeRef<T>(args...) is the only construction entry — the
 * inner block carries the strong count and stores T inline, so the
 * whole thing is a single heap allocation.
 *
 * Choose Ref<T> when:
 *   - Ownership is shared (the same T is reachable from several places).
 *   - You want sizeof == sizeof(T*) per handle, not the 2-word
 *     std::shared_ptr layout.
 *   - You don't need weak references or cycle breaking. (libx++'s Ref
 *     deliberately omits Weak<T> in this version — keep your graphs
 *     acyclic, or move to xpp::Own<T> + raw borrowing.)
 *
 * Choose Own<T> instead when ownership is unique.
 * Choose NonNull<T> / Option<NonNull<T>> for non-owning references.
 *
 * Thread safety:
 *   This version's strong count is NOT atomic. Sharing a single Ref<T>
 *   across threads (or having Ref<T>s on multiple threads that point
 *   at the same T) is undefined behaviour. A future AtomicRef<T> may
 *   add the atomic variant on demand; today, keep Ref<T> within a
 *   single thread or wrap moves through an explicit handoff.
 *
 * C++11-compatible. Header-only. Trivially nothing — Ref carries
 * non-trivial copy/destroy semantics that ref-count the inner.
 */

#ifndef XPP_REF_H
#define XPP_REF_H

#include <xpp/option.h>
#include <xpp/panic.h>

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace xpp {

template <class T> class Ref;

namespace _ {

/**
 * @brief Heap-allocated control block + value, the storage Ref<T>
 *        points at. Single allocation; the strong count lives next to
 *        the value so cache lines are shared.
 *
 * Layout matters: `value` follows the count, both `alignas(T)`. The
 * compiler folds `&inner->value` into a constant-offset addressing
 * mode, so deref() is as cheap as for a raw T*.
 *
 * RefCount is a plain `size_t` — Ref<T> is single-thread for now. If
 * AtomicRef<T> arrives later it lives in its own header with its own
 * RefInner<T> using std::atomic<size_t>, so this struct stays small.
 */
template <class T> struct RefInner {
  size_t strong;
  T      value;

  template <class... Args>
  explicit RefInner(Args &&...args) : strong(1), value(std::forward<Args>(args)...) {}
};

} // namespace _

/**
 * @brief Non-null shared-owning reference-counted handle to a
 *        heap-allocated T.
 *
 * Always points at a live T whose RefInner has strong >= 1. Move-from
 * leaves the source in an unspecified state (internally null) — using
 * the moved-from Ref is undefined; only destruction is safe. Same
 * contract as std::unique_ptr's moved-from state.
 *
 * Use makeRef<T>(args...) to construct. There is no public
 * constructor from a raw T* — Ref always owns the allocation it was
 * born with, and a stray pointer can't be retro-fitted with a count.
 */
template <class T> class Ref {
public:
  using value_type = T;

  /**
   * @brief Copy constructor: +1 on the shared count.
   *
   * Implicit on purpose. For hot-path code where "this is a new
   * owner" should be visually obvious, call `.clone()` instead — it
   * does the same thing but reads as deliberate.
   */
  Ref(const Ref &o) noexcept : m_inner(o.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    ++m_inner->strong;
  }

  /**
   * @brief Move constructor: zero count changes; the source becomes
   *        invalid (UB to use other than to destroy).
   */
  Ref(Ref &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  /**
   * @brief Covariant copy: Ref<Derived> → Ref<Base>.
   *
   * Static-checks U is convertible to T; the inner block's layout is
   * unchanged, only the pointer type narrows.
   */
  template <class U, typename = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Ref(const Ref<U> &o) noexcept : m_inner(reinterpret_cast<_::RefInner<T> *>(o.innerRaw())) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    ++m_inner->strong;
  }

  /**
   * @brief Covariant move: Ref<Derived> → Ref<Base>.
   */
  template <class U, typename = typename std::enable_if<std::is_convertible<U *, T *>::value>::type>
  Ref(Ref<U> &&o) noexcept : m_inner(reinterpret_cast<_::RefInner<T> *>(o.innerRaw())) {
    o.innerRawReset();
  }

  Ref &operator=(const Ref &o) noexcept {
    if (this != &o) {
      Ref tmp(o);
      swap(tmp);
    }
    return *this;
  }

  Ref &operator=(Ref &&o) noexcept {
    if (this != &o) {
      Ref tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }

  ~Ref() noexcept {
    if (m_inner) decRefAndMaybeDestroy();
  }

  /**
   * @brief Explicit +1, Rust-style.
   *
   * Identical to copy construction; provided so hot-path code can
   * write `auto b = a.clone()` and make the count bump visible. The
   * implicit copy constructor remains available — pick whichever
   * makes the call site read best.
   */
  Ref clone() const noexcept {
    return Ref(*this);
  }

  /**
   * @brief Strong reference count (debugging only).
   *
   * Don't branch on this in production code — a same-thread Ref::clone
   * can change it between the read and any decision you make. Mostly
   * useful for assertions and tests ("did this drop?").
   */
  size_t useCount() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    return m_inner->strong;
  }

  T &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    return m_inner->value;
  }
  T *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    return &m_inner->value;
  }

  /**
   * @brief Borrow the contained T as a raw pointer.
   *
   * The pointer is valid as long as some Ref to this object exists;
   * do NOT store it past the lifetime of the Refs.
   */
  T *get() const noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    return &m_inner->value;
  }

  void swap(Ref &o) noexcept {
    _::RefInner<T> *tmp = m_inner;
    m_inner             = o.m_inner;
    o.m_inner           = tmp;
  }

  /* Pointer-identity equality. Two Refs compare equal iff they point
   * at the same inner allocation. */
  bool operator==(const Ref &o) const noexcept {
    return m_inner == o.m_inner;
  }
  bool operator!=(const Ref &o) const noexcept {
    return m_inner != o.m_inner;
  }

  /* ── internals exposed only to friends / templates ───────────────── */

  // Used by covariant ctor of Ref<Other>; do not call directly.
  _::RefInner<T> *innerRaw() const noexcept {
    return m_inner;
  }
  void innerRawReset() noexcept {
    m_inner = nullptr;
  }

private:
  // Constructed only by makeRef and the friend Option specialization.
  explicit Ref(_::RefInner<T> *inner) noexcept : m_inner(inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
  }

  void decRefAndMaybeDestroy() noexcept {
    if (--m_inner->strong == 0) {
      // Destroy in place, then deallocate the inner block.
      m_inner->~RefInner();
      ::operator delete(m_inner);
    }
    m_inner = nullptr;
  }

  _::RefInner<T> *m_inner;

  template <class U, class... Args> friend Ref<U> makeRef(Args &&...args);
  template <class U> friend class Ref;
  friend class Option<Ref<T>>;
};

/**
 * @brief Construct a Ref<T> in place.
 *
 * Single heap allocation: the inner block holds the strong count
 * (initialised to 1) and constructs T from @p args.
 *
 * libx++ does not use exceptions for control flow (see README), so
 * makeRef expects T's constructor not to throw. If T can fail to
 * construct, model that with a static factory returning
 * Result<T, Error> and call makeRef on the unwrapped success path.
 *
 * The only public construction entry point. Ref has no constructor
 * from a raw T* — if you have a T and want it ref-counted, pass it
 * (or its ctor args) to makeRef.
 */
template <class T, class... Args> Ref<T> makeRef(Args &&...args) {
  void           *mem   = ::operator new(sizeof(_::RefInner<T>));
  _::RefInner<T> *inner = ::new (mem) _::RefInner<T>(std::forward<Args>(args)...);
  return Ref<T>(inner);
}

template <class T> void swap(Ref<T> &a, Ref<T> &b) noexcept {
  a.swap(b);
}

/**
 * @brief Niche-optimized Option<Ref<T>>. Storage is a single
 *        RefInner<T>* whose nullptr value means None.
 *
 *   sizeof(Option<Ref<T>>) == sizeof(T*)
 *
 * Owns a +1 on the inner block whenever it is Some, releases it on
 * destruction or when overwritten with None. Unlike the generic
 * Option<T>, this specialisation does not store a bool tag — the
 * pointer's null-ness IS the tag.
 *
 * Copies behave like Ref copies: each new Some increments the count.
 * Moves do not touch the count.
 */
template <class T> class Option<Ref<T>> {
public:
  using value_type = Ref<T>;

  /* ── ctors / dtor / assignment ───────────────────────────────────── */

  constexpr Option() noexcept : m_inner(nullptr) {}
  constexpr Option(None) noexcept : m_inner(nullptr) {}

  Option(const Ref<T> &r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
    ++m_inner->strong;
  }
  Option(Ref<T> &&r) noexcept : m_inner(r.m_inner) {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Ref must own an inner");
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
    Option tmp; // empty
    swap(tmp);
    return *this;
  }

  ~Option() noexcept {
    decRefIfSome();
  }

  /* ── predicates ──────────────────────────────────────────────────── */

  bool isSome() const noexcept {
    return m_inner != nullptr;
  }
  bool isNone() const noexcept {
    return m_inner == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_inner != nullptr;
  }

  /* ── unwrap (returns Ref<T> by value; the Option becomes None) ───── */

  Ref<T> unwrap() && {
    XPP_ASSERT(m_inner != nullptr, "unwrap() on None Option");
    _::RefInner<T> *taken = m_inner;
    m_inner               = nullptr;
    return Ref<T>(taken);
  }

  Ref<T> unwrapUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_inner != nullptr, "internal: Option must be Some");
    _::RefInner<T> *taken = m_inner;
    m_inner               = nullptr;
    return Ref<T>(taken);
  }

  /**
   * @brief Take the Ref out, leaving None behind.
   *
   * Identical to `std::move(opt).unwrap()` but reads naturally when
   * the Option is an lvalue (e.g. an element in a container).
   */
  Ref<T> take() {
    return std::move(*this).unwrap();
  }

  void swap(Option &o) noexcept {
    _::RefInner<T> *tmp = m_inner;
    m_inner             = o.m_inner;
    o.m_inner           = tmp;
  }

private:
  void decRefIfSome() noexcept {
    if (m_inner) {
      if (--m_inner->strong == 0) {
        m_inner->~RefInner();
        ::operator delete(m_inner);
      }
      m_inner = nullptr;
    }
  }

  _::RefInner<T> *m_inner;
};

template <class T> void swap(Option<Ref<T>> &a, Option<Ref<T>> &b) noexcept {
  a.swap(b);
}

/* ── invariants pinned at compile time ────────────────────────────── */

static_assert(sizeof(Ref<int>) == sizeof(int *), "Ref<T> must be sizeof(T*)");
static_assert(sizeof(Option<Ref<int>>) == sizeof(int *),
              "Option<Ref<T>> niche optimisation broken");

} // namespace xpp

#endif // XPP_REF_H
