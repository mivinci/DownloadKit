/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nonnull_own.h - NonNullOwn<T, Deleter>: a guaranteed-non-null
 *                 owning smart pointer (move-only RAII), plus a
 *                 niche-optimized Option<NonNullOwn<T, D>>.
 *
 * sizeof(NonNullOwn<T>)         == sizeof(T*)   (default_delete is empty → EBO)
 * sizeof(Option<NonNullOwn<T>>) == sizeof(T*)   ← matches Rust's Option<Box<T>>
 *
 * For stateful Deleters, sizeof grows by sizeof(Deleter) (rounded for
 * alignment), matching std::unique_ptr's storage strategy.
 *
 * The "moved-from" state of NonNullOwn technically holds a null
 * pointer, violating the public invariant. This is treated as a private
 * implementation detail visible only to the destructor (which guards
 * on null). Calling get() / operator* / operator-> on a moved-from
 * value is undefined — see std::unique_ptr's analogous contract.
 *
 * C++14-compatible. Header-only. No reset() — the type cannot be null.
 */

#ifndef XPP_NONNULL_OWN_H
#define XPP_NONNULL_OWN_H

#include "nonnull.h"
#include "option.h"
#include "panic.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace xpp {
namespace _ {

/**
 * @brief T* + Deleter pair with empty-base optimization for empty Deleters.
 *
 * Two specializations keyed on `is_empty<D> && !is_final<D>`:
 *   - empty + non-final → inherit privately from D, sizeof == sizeof(T*)
 *   - otherwise         → store D as a member, sizeof == sizeof(T*) + sizeof(D)
 *
 * Mirrors the strategy used by libc++ / libstdc++ / MSVC STL inside
 * std::unique_ptr.
 */
template <class T, class D,
          bool Empty = std::is_empty<D>::value && !std::is_final<D>::value>
struct CompressedPair {
  T *p;
  D  d;

  CompressedPair() noexcept(std::is_nothrow_default_constructible<D>::value)
      : p(nullptr), d() {}
  CompressedPair(T *p_, D d_) noexcept(std::is_nothrow_move_constructible<D>::value)
      : p(p_), d(std::move(d_)) {}

  D       &deleter() noexcept       { return d; }
  const D &deleter() const noexcept { return d; }
};

template <class T, class D>
struct CompressedPair<T, D, true> : private D {
  T *p;

  CompressedPair() noexcept(std::is_nothrow_default_constructible<D>::value)
      : D(), p(nullptr) {}
  CompressedPair(T *p_, D d_) noexcept(std::is_nothrow_move_constructible<D>::value)
      : D(std::move(d_)), p(p_) {}

  D       &deleter() noexcept       { return *this; }
  const D &deleter() const noexcept { return *this; }
};

} // namespace _

/**
 * @brief A non-null owning pointer to T with a custom Deleter.
 *
 * Semantically equivalent to a std::unique_ptr<T, D> that is never null.
 * Move-only. No reset (would imply nullable storage).
 *
 * Construction:
 *   - newUnchecked(T*, D = D{})   — caller asserts non-null (debug-checked)
 *   - from(T*, D = D{})           — checked, returns Option<NonNullOwn>
 *
 * @tparam T        Pointee type. T = void supported (operator*, ->
 *                  SFINAE-removed).
 * @tparam Deleter  Function-object-like type called on the held pointer
 *                  in the destructor. Defaults to std::default_delete<T>.
 */
template <class T, class Deleter = std::default_delete<T>>
class NonNullOwn {
  using Storage = _::CompressedPair<T, Deleter>;

public:
  using element_type = T;
  using deleter_type = Deleter;
  using pointer      = T *;

  NonNullOwn()                                  = delete;
  NonNullOwn(const NonNullOwn &)             = delete;
  NonNullOwn &operator=(const NonNullOwn &)  = delete;

  /** @brief Move ctor. Source becomes a "destruction-only" husk. */
  NonNullOwn(NonNullOwn &&o) noexcept
      : m_storage(o.m_storage.p, std::move(o.m_storage.deleter())) {
    o.m_storage.p = nullptr;
  }

  /**
   * @brief Covariant move ctor: NonNullOwn<Derived, E> → NonNullOwn<Base, D>.
   *
   * Mirrors std::unique_ptr<U, E> → std::unique_ptr<T, D>: enabled when U*
   * is convertible to T* and E&& is convertible to D. This covers the
   * common case `default_delete<Derived>` → `default_delete<Base>`.
   */
  template <class U, class E,
            class = typename std::enable_if<
                std::is_convertible<U *, T *>::value && !std::is_same<U, T>::value &&
                std::is_convertible<E &&, Deleter>::value>::type>
  NonNullOwn(NonNullOwn<U, E> &&o) noexcept
      : m_storage(static_cast<T *>(o.m_storage.p),
                  static_cast<Deleter>(std::move(o.m_storage.deleter()))) {
    o.m_storage.p = nullptr;
  }

  /** @brief Move assignment. Old target is deleted; source becomes husk. */
  NonNullOwn &operator=(NonNullOwn &&o) noexcept {
    if (this != &o) {
      reset_internal();
      m_storage.p         = o.m_storage.p;
      m_storage.deleter() = std::move(o.m_storage.deleter());
      o.m_storage.p       = nullptr;
    }
    return *this;
  }

  ~NonNullOwn() {
    reset_internal();
  }

  /** @brief Wrap a raw pointer; caller asserts non-null. */
  static NonNullOwn newUnchecked(T *p, Deleter d = Deleter{}) noexcept {
    XPP_DEBUG_ASSERT(p != nullptr, "NonNullOwn::newUnchecked: pointer is null");
    return NonNullOwn(p, std::move(d), _PrivateTag{});
  }

  /** @brief Checked factory. Defined out-of-line below the Option specialization. */
  static Option<NonNullOwn> from(T *p, Deleter d = Deleter{}) noexcept;

  T *get() const noexcept {
    return m_storage.p;
  }

  template <class U = T,
            class   = typename std::enable_if<!std::is_void<U>::value>::type>
  U &operator*() const noexcept {
    return *m_storage.p;
  }

  template <class U = T,
            class   = typename std::enable_if<!std::is_void<U>::value>::type>
  U *operator->() const noexcept {
    return m_storage.p;
  }

  Deleter &getDeleter() noexcept {
    return m_storage.deleter();
  }
  const Deleter &getDeleter() const noexcept {
    return m_storage.deleter();
  }

  /** @brief Borrow as a non-owning NonNull view. */
  NonNull<T> asNonNull() const noexcept {
    return NonNull<T>::newUnchecked(m_storage.p);
  }

  /**
   * @brief Relinquish ownership; return the raw pointer.
   *
   * Mirrors std::unique_ptr::release. Consuming (rvalue-only) so we can
   * never leave a NonNullOwn observable in a "null" state.
   */
  T *release() && noexcept {
    T *r          = m_storage.p;
    m_storage.p   = nullptr;
    return r;
  }

private:
  struct _PrivateTag {};
  NonNullOwn(T *p, Deleter d, _PrivateTag) noexcept
      : m_storage(p, std::move(d)) {}

  void reset_internal() noexcept {
    if (m_storage.p) {
      m_storage.deleter()(m_storage.p);
      m_storage.p = nullptr;
    }
  }

  Storage m_storage;

  // Allow Option<NonNullOwn<...>> (any instantiation) to access storage —
  // needed by both the matching specialization's takeOwned() and by the
  // covariant ctor on Option<NonNullOwn<Base>> reaching into a moved-from
  // Option<NonNullOwn<Derived>>.
  template <class> friend class Option;
  // Allow covariant ctor to access another instantiation's storage.
  template <class, class> friend class NonNullOwn;
};

/**
 * @brief Niche-optimized Option<NonNullOwn<T, D>>. Storage is the same
 *        pair as NonNullOwn itself; nullptr ↔ None.
 *
 * Move-only (mirrors stored type). The destructor invokes the deleter
 * iff the storage pointer is non-null.
 *
 * unwrap() is asymmetric:
 *   - const &  → returns T* (a non-owning borrow). Cannot return owning
 *                NonNullOwn (move-only) and cannot return a reference
 *                because no owning value lives in storage.
 *   - &&       → returns NonNullOwn<T, D> by move (consumes).
 *
 * Combinators map() / andThen() / filter() / inspect():
 *   - const & overload  → fn receives NonNull<T> (non-owning view)
 *   - && overload       → fn receives NonNullOwn<T, D>&& (consumes)
 */
template <class T, class Deleter>
class Option<NonNullOwn<T, Deleter>> {
  using Storage = _::CompressedPair<T, Deleter>;

public:
  using value_type = NonNullOwn<T, Deleter>;

  Option() noexcept                  : m_storage() {}
  Option(None) noexcept              : m_storage() {}
  Option(NonNullOwn<T, Deleter> &&u) noexcept
      : m_storage(u.m_storage.p, std::move(u.m_storage.deleter())) {
    u.m_storage.p = nullptr;
  }

  /** @brief Covariant: adopt NonNullOwn<Derived, E>. */
  template <class U, class E,
            class = typename std::enable_if<
                std::is_convertible<U *, T *>::value && !std::is_same<U, T>::value &&
                std::is_convertible<E &&, Deleter>::value>::type>
  Option(NonNullOwn<U, E> &&u) noexcept
      : m_storage(static_cast<T *>(u.m_storage.p),
                  static_cast<Deleter>(std::move(u.m_storage.deleter()))) {
    u.m_storage.p = nullptr;
  }

  Option(const Option &)            = delete;
  Option &operator=(const Option &) = delete;

  Option(Option &&o) noexcept
      : m_storage(o.m_storage.p, std::move(o.m_storage.deleter())) {
    o.m_storage.p = nullptr;
  }

  /** @brief Covariant: adopt Option<NonNullOwn<Derived, E>>. */
  template <class U, class E,
            class = typename std::enable_if<
                std::is_convertible<U *, T *>::value && !std::is_same<U, T>::value &&
                std::is_convertible<E &&, Deleter>::value>::type>
  Option(Option<NonNullOwn<U, E>> &&o) noexcept
      : m_storage(static_cast<T *>(o.m_storage.p),
                  static_cast<Deleter>(std::move(o.m_storage.deleter()))) {
    o.m_storage.p = nullptr;
  }
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      reset_internal();
      m_storage.p         = o.m_storage.p;
      m_storage.deleter() = std::move(o.m_storage.deleter());
      o.m_storage.p       = nullptr;
    }
    return *this;
  }
  Option &operator=(None) noexcept {
    reset_internal();
    return *this;
  }

  ~Option() {
    reset_internal();
  }

  bool isSome() const noexcept {
    return m_storage.p != nullptr;
  }
  bool isNone() const noexcept {
    return m_storage.p == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_storage.p != nullptr;
  }

  /* ── unwrap (asymmetric: borrow vs consume) ─────────────────────── */

  T *unwrap() const & {
    XPP_ASSERT(m_storage.p != nullptr, "unwrap() on None Option");
    return m_storage.p;
  }
  NonNullOwn<T, Deleter> unwrap() && {
    XPP_ASSERT(m_storage.p != nullptr, "unwrap() on None Option");
    return takeOwned();
  }

  T *unwrapUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(m_storage.p, "internal: Option must be Some");
    return m_storage.p;
  }
  NonNullOwn<T, Deleter> unwrapUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_storage.p, "internal: Option must be Some");
    return takeOwned();
  }

  NonNullOwn<T, Deleter> unwrapOr(NonNullOwn<T, Deleter> &&fallback) && {
    if (m_storage.p) return takeOwned();
    return std::move(fallback);
  }

  Option take() noexcept {
    return std::move(*this);
  }

  T *expect(const char *msg) const & {
    XPP_ASSERT(m_storage.p != nullptr, "expect: %s", msg);
    return m_storage.p;
  }
  NonNullOwn<T, Deleter> expect(const char *msg) && {
    XPP_ASSERT(m_storage.p != nullptr, "expect: %s", msg);
    return takeOwned();
  }

  /* ── combinators ────────────────────────────────────────────────── */

  template <class Func>
  auto map(Func &&fn) const & -> Option<decltype(fn(std::declval<NonNull<T>>()))> {
    using U = decltype(fn(std::declval<NonNull<T>>()));
    return m_storage.p ? Option<U>(fn(NonNull<T>::newUnchecked(m_storage.p)))
                       : Option<U>(none);
  }
  template <class Func>
  auto map(Func &&fn) && -> Option<decltype(fn(std::declval<NonNullOwn<T, Deleter> &&>()))> {
    using U = decltype(fn(std::declval<NonNullOwn<T, Deleter> &&>()));
    if (!m_storage.p) return Option<U>(none);
    NonNullOwn<T, Deleter> owned = takeOwned();
    return Option<U>(fn(std::move(owned)));
  }

  template <class Func>
  auto andThen(Func &&fn) && -> decltype(fn(std::declval<NonNullOwn<T, Deleter> &&>())) {
    using R = decltype(fn(std::declval<NonNullOwn<T, Deleter> &&>()));
    if (!m_storage.p) return R(none);
    NonNullOwn<T, Deleter> owned = takeOwned();
    return fn(std::move(owned));
  }

  template <class Func> Option orElse(Func &&fn) && {
    if (m_storage.p) return std::move(*this);
    return fn();
  }

  template <class Func> NonNullOwn<T, Deleter> unwrapOrElse(Func &&fn) && {
    if (m_storage.p) return takeOwned();
    return fn();
  }

  template <class Func> Option filter(Func &&pred) && {
    if (m_storage.p && pred(NonNull<T>::newUnchecked(m_storage.p))) {
      return std::move(*this);
    }
    reset_internal();
    return none;
  }

  template <class Func> const Option &inspect(Func &&fn) const & {
    if (m_storage.p) fn(NonNull<T>::newUnchecked(m_storage.p));
    return *this;
  }
  template <class Func> Option inspect(Func &&fn) && {
    if (m_storage.p) fn(NonNull<T>::newUnchecked(m_storage.p));
    return std::move(*this);
  }

private:
  void reset_internal() noexcept {
    if (m_storage.p) {
      m_storage.deleter()(m_storage.p);
      m_storage.p = nullptr;
    }
  }

  /** Move ownership out of storage; storage left empty. Caller has checked p != null. */
  NonNullOwn<T, Deleter> takeOwned() noexcept {
    NonNullOwn<T, Deleter> r = NonNullOwn<T, Deleter>::newUnchecked(
        m_storage.p, std::move(m_storage.deleter()));
    m_storage.p = nullptr;
    return r;
  }

  Storage m_storage;

  // Allow covariant ctor to access another instantiation's storage.
  template <class> friend class Option;
};

/* ── NonNullOwn<T, D>::from definition ─────────────────────────────── */

template <class T, class D>
inline Option<NonNullOwn<T, D>> NonNullOwn<T, D>::from(T *p, D d) noexcept {
  if (!p) return Option<NonNullOwn<T, D>>(none);
  return Option<NonNullOwn<T, D>>(
      NonNullOwn<T, D>(p, std::move(d), _PrivateTag{}));
}

/* ── Compile-time size guarantees ────────────────────────────────────── */

static_assert(sizeof(NonNullOwn<int>) == sizeof(int *),
              "NonNullOwn<T, default_delete> must be sizeof(T*) via EBO");
static_assert(sizeof(Option<NonNullOwn<int>>) == sizeof(int *),
              "Option<NonNullOwn<T, default_delete>> niche broken");

} // namespace xpp

#endif // XPP_NONNULL_OWN_H
