/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nonnull.h - NonNull<T>: a guaranteed-non-null pointer wrapper, plus
 *             a niche-optimized Option<NonNull<T>> partial specialization.
 *
 * sizeof(NonNull<T>)         == sizeof(T*)
 * sizeof(Option<NonNull<T>>) == sizeof(T*)   ← matches Rust's Option<&T> /
 *                                              Option<NonNull<T>> / Option<Box<T>>
 *
 * Compared to Option<T*> (which is 16 bytes due to the bool tag), this
 * saves 8 bytes per slot when the caller can prove non-nullness at the
 * type level. Use it for hot data structures (intrusive lists, trees)
 * and API boundaries where "must be non-null" is a documented contract.
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_NONNULL_H
#define XPP_NONNULL_H

#include <xpp/option.h>
#include <xpp/panic.h>

#include <type_traits>
#include <utility>

namespace xpp {

template <class T> class NonNull;

/**
 * @brief A non-null pointer to T. Class invariant: get() != nullptr.
 *
 * NonNull is a thin wrapper around T*. Construction always proves
 * non-nullness:
 *   - NonNull(T &ref)            — bind to an existing referent (always safe)
 *   - NonNull::newUnchecked(T*)  — caller asserts non-null (debug-checked)
 *   - NonNull::from(T*)          — checked, returns Option<NonNull<T>>
 *
 * There is no constructor that takes a raw T* and panics on null —
 * users must go through from() to acknowledge the check.
 *
 * @tparam T  Pointee type. T = void is supported, but operator* and
 *            operator-> are SFINAE-removed in that case.
 */
template <class T> class NonNull {
public:
  using element_type = T;

  /**
   * @brief Bind to an existing referent. Always safe.
   *
   * SFINAE constraints:
   *   1. T is not void (`void&` is ill-formed).
   *   2. U is the same as T. Without this guard, GCC's overload
   *      resolution sometimes prefers this templated ctor over the
   *      implicit copy ctor when an lvalue NonNull<T> is passed by
   *      value (e.g. as a function parameter), then fails to
   *      compile because it tries to initialise m_ptr (a T*) from a
   *      NonNull<T>*. Clang silently picks the copy ctor in the
   *      same situation. Pinning U = T resolves the ambiguity in
   *      GCC's favour, no behavioural change anywhere else.
   */
  template <
    class U = T,
    class   = typename std::enable_if<!std::is_void<U>::value && std::is_same<U, T>::value>::type>
  explicit NonNull(U &ref) noexcept : m_ptr(&ref) {}

  NonNull(const NonNull &)            = default;
  NonNull(NonNull &&)                 = default;
  NonNull &operator=(const NonNull &) = default;
  NonNull &operator=(NonNull &&)      = default;
  ~NonNull()                          = default;

  /**
   * @brief Wrap a raw pointer; caller asserts it is non-null.
   *
   * Mirrors Rust's NonNull::new_unchecked. Debug builds verify the
   * precondition; release builds elide the check entirely.
   */
  static NonNull newUnchecked(T *p) noexcept {
    XPP_DEBUG_ASSERT(p != nullptr, "NonNull::newUnchecked: pointer is null");
    return NonNull(p, _PrivateTag{});
  }

  /**
   * @brief Checked factory: nullptr -> none, non-null -> Some(NonNull).
   *
   * Mirrors Rust's NonNull::new. The only safe entry point from a raw
   * pointer of unknown nullness.
   *
   * Defined out-of-line below the Option<NonNull<T>> specialization.
   */
  static Option<NonNull> from(T *p) noexcept;

  /** @brief Get the underlying raw pointer. Never null. */
  T *get() const noexcept {
    return m_ptr;
  }

  /** @brief Dereference. Removed via SFINAE for T = void. */
  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U &operator*() const noexcept {
    return *m_ptr;
  }

  /** @brief Member access. Removed via SFINAE for T = void. */
  template <class U = T, class = typename std::enable_if<!std::is_void<U>::value>::type>
  U *operator->() const noexcept {
    return m_ptr;
  }

  friend bool operator==(NonNull a, NonNull b) noexcept {
    return a.m_ptr == b.m_ptr;
  }
  friend bool operator!=(NonNull a, NonNull b) noexcept {
    return a.m_ptr != b.m_ptr;
  }

private:
  struct _PrivateTag {};
  NonNull(T *p, _PrivateTag) noexcept : m_ptr(p) {}

  T *m_ptr;

  friend class Option<NonNull<T>>;
};

/**
 * @brief Niche-optimized Option<NonNull<T>>. Storage is a single T*.
 *
 * nullptr ↔ None, any non-null pointer ↔ Some(NonNull(p)). No bool
 * tag. Trivially copyable. sizeof(Option<NonNull<T>>) == sizeof(T*).
 *
 * unwrap() returns NonNull<T> by value on all overloads — there is no
 * stored NonNull to take a reference to (storage is just a pointer),
 * and NonNull is sizeof(T*) so by-value is free.
 */
template <class T> class Option<NonNull<T>> {
public:
  using value_type = NonNull<T>;

  constexpr Option() noexcept : m_ptr(nullptr) {}
  constexpr Option(None) noexcept : m_ptr(nullptr) {}
  Option(NonNull<T> nn) noexcept : m_ptr(nn.get()) {}

  Option(const Option &) = default;
  Option(Option &&o) noexcept : m_ptr(o.m_ptr) {
    o.m_ptr = nullptr;
  }
  Option &operator=(const Option &) = default;
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      m_ptr   = o.m_ptr;
      o.m_ptr = nullptr;
    }
    return *this;
  }
  Option &operator=(None) noexcept {
    m_ptr = nullptr;
    return *this;
  }
  ~Option() = default;

  bool isSome() const noexcept {
    return m_ptr != nullptr;
  }
  bool isNone() const noexcept {
    return m_ptr == nullptr;
  }
  explicit operator bool() const noexcept {
    return m_ptr != nullptr;
  }

  /* ── unwrap (returns NonNull<T> by value on every overload) ─────── */

  NonNull<T> unwrap() const & {
    XPP_ASSERT(m_ptr != nullptr, "unwrap() on None Option");
    return NonNull<T>::newUnchecked(m_ptr);
  }
  NonNull<T> unwrap() && {
    XPP_ASSERT(m_ptr != nullptr, "unwrap() on None Option");
    NonNull<T> r = NonNull<T>::newUnchecked(m_ptr);
    m_ptr        = nullptr;
    return r;
  }

  NonNull<T> unwrapUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(m_ptr != nullptr, "internal: Option must be Some");
    return NonNull<T>::newUnchecked(m_ptr);
  }
  NonNull<T> unwrapUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_ptr != nullptr, "internal: Option must be Some");
    NonNull<T> r = NonNull<T>::newUnchecked(m_ptr);
    m_ptr        = nullptr;
    return r;
  }

  NonNull<T> unwrapOr(NonNull<T> fallback) const & noexcept {
    return m_ptr ? NonNull<T>::newUnchecked(m_ptr) : fallback;
  }
  NonNull<T> unwrapOr(NonNull<T> fallback) && noexcept {
    if (m_ptr) {
      NonNull<T> r = NonNull<T>::newUnchecked(m_ptr);
      m_ptr        = nullptr;
      return r;
    }
    return fallback;
  }

  Option take() noexcept {
    Option r = *this;
    m_ptr    = nullptr;
    return r;
  }

  NonNull<T> expect(const char *msg) const & {
    XPP_ASSERT(m_ptr != nullptr, "expect: %s", msg);
    return NonNull<T>::newUnchecked(m_ptr);
  }
  NonNull<T> expect(const char *msg) && {
    XPP_ASSERT(m_ptr != nullptr, "expect: %s", msg);
    NonNull<T> r = NonNull<T>::newUnchecked(m_ptr);
    m_ptr        = nullptr;
    return r;
  }

  /* ── combinators (fn takes NonNull<T> by value) ──────────────────── */

  template <class Func>
  auto map(Func &&fn) const & -> Option<decltype(fn(std::declval<NonNull<T>>()))> {
    using U = decltype(fn(std::declval<NonNull<T>>()));
    return m_ptr ? Option<U>(fn(NonNull<T>::newUnchecked(m_ptr))) : Option<U>(none);
  }
  template <class Func> auto map(Func &&fn) && -> Option<decltype(fn(std::declval<NonNull<T>>()))> {
    using U = decltype(fn(std::declval<NonNull<T>>()));
    if (!m_ptr) return Option<U>(none);
    NonNull<T> p = NonNull<T>::newUnchecked(m_ptr);
    m_ptr        = nullptr;
    return Option<U>(fn(p));
  }

  template <class Func>
  auto andThen(Func &&fn) const & -> decltype(fn(std::declval<NonNull<T>>())) {
    using R = decltype(fn(std::declval<NonNull<T>>()));
    return m_ptr ? fn(NonNull<T>::newUnchecked(m_ptr)) : R(none);
  }
  template <class Func> auto andThen(Func &&fn) && -> decltype(fn(std::declval<NonNull<T>>())) {
    using R = decltype(fn(std::declval<NonNull<T>>()));
    if (!m_ptr) return R(none);
    NonNull<T> p = NonNull<T>::newUnchecked(m_ptr);
    m_ptr        = nullptr;
    return fn(p);
  }

  template <class Func> Option orElse(Func &&fn) const & {
    return m_ptr ? *this : fn();
  }
  template <class Func> Option orElse(Func &&fn) && {
    if (m_ptr) {
      Option r = *this;
      m_ptr    = nullptr;
      return r;
    }
    return fn();
  }

  template <class Func> NonNull<T> unwrapOrElse(Func &&fn) && {
    if (m_ptr) {
      NonNull<T> r = NonNull<T>::newUnchecked(m_ptr);
      m_ptr        = nullptr;
      return r;
    }
    return fn();
  }

  template <class Func> Option filter(Func &&pred) && {
    if (m_ptr && pred(NonNull<T>::newUnchecked(m_ptr))) {
      Option r = *this;
      m_ptr    = nullptr;
      return r;
    }
    m_ptr = nullptr;
    return none;
  }

  template <class Func> Option &inspect(Func &&fn) & {
    if (m_ptr) fn(NonNull<T>::newUnchecked(m_ptr));
    return *this;
  }
  template <class Func> const Option &inspect(Func &&fn) const & {
    if (m_ptr) fn(NonNull<T>::newUnchecked(m_ptr));
    return *this;
  }
  template <class Func> Option inspect(Func &&fn) && {
    if (m_ptr) fn(NonNull<T>::newUnchecked(m_ptr));
    return std::move(*this);
  }

private:
  T *m_ptr;
};

/* ── NonNull<T>::from definition (Option<NonNull<T>> now complete) ── */

template <class T> inline Option<NonNull<T>> NonNull<T>::from(T *p) noexcept {
  return p ? Option<NonNull<T>>(NonNull<T>(p, _PrivateTag{})) : Option<NonNull<T>>(none);
}

/* ── Compile-time size guarantees ───────────────────────────────────── */

static_assert(sizeof(NonNull<int>) == sizeof(int *), "NonNull<T> must be sizeof(T*)");
static_assert(sizeof(Option<NonNull<int>>) == sizeof(int *),
              "Option<NonNull<T>> niche optimization broken");

} // namespace xpp

#endif // XPP_NONNULL_H
