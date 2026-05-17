/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * option.h - Option<T>: a value or nothing (like std::optional).
 *
 * C++11-compatible.
 */

#ifndef XPP_OPTION_H
#define XPP_OPTION_H

#include <xpp/panic.h>

#include <utility>

namespace xpp {

/** Forward declaration so Option::okOr / okOrElse can name Result. */
template <class T, typename E> class Result;

/**
 * @brief Tag for constructing an empty Option.
 *
 * Usage: Option<int> a(none);
 */
struct None {
  explicit None() = default;
};

constexpr None none{};

/**
 * @brief A value or nothing — like Rust's Option or C++17's std::optional.
 *
 * @tparam T  The value type.
 */
template <class T> class Option {
public:
  /** Underlying value type, following std::optional convention. */
  using value_type = T;

  /** @brief Construct an empty Option. */
  constexpr Option() noexcept : m_hasValue(false) {}

  /** @brief Construct an empty Option from none. */
  constexpr Option(None) noexcept : m_hasValue(false) {}

  /** @brief Construct with a value. */
  Option(const T &val) : m_hasValue(true) {
    new (&m_storage) T(val);
  }

  Option(T &&val) noexcept : m_hasValue(true) {
    new (&m_storage) T(std::move(val));
  }

  /** @brief Copy constructor. */
  Option(const Option &o) : m_hasValue(o.m_hasValue) {
    if (m_hasValue) new (&m_storage) T(o.unwrap());
  }

  /** @brief Move constructor. Source is left empty. */
  Option(Option &&o) noexcept : m_hasValue(o.m_hasValue) {
    if (m_hasValue) {
      new (&m_storage) T(std::move(o.unwrap()));
      o.clear();
    }
  }

  /** @brief Destructor. Destroys the held value if present. */
  ~Option() {
    clear();
  }

  /** @brief Copy assignment. */
  Option &operator=(const Option &o) {
    if (this != &o) {
      clear();
      m_hasValue = o.m_hasValue;
      if (m_hasValue) new (&m_storage) T(o.unwrap());
    }
    return *this;
  }

  /** @brief Move assignment. Source is left empty. */
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      clear();
      m_hasValue = o.m_hasValue;
      if (m_hasValue) {
        new (&m_storage) T(std::move(o.unwrap()));
        o.clear();
      }
    }
    return *this;
  }

  /** @brief Assign none, destroying any held value. */
  Option &operator=(None) noexcept {
    clear();
    return *this;
  }

  /** @brief True if this holds a value. */
  bool isSome() const noexcept {
    return m_hasValue;
  }

  /** @brief True if this is empty. */
  bool isNone() const noexcept {
    return !m_hasValue;
  }

  /** @brief Bool conversion: true if some. */
  explicit operator bool() const noexcept {
    return m_hasValue;
  }

  /**
   * @brief Get the held value, aborting if empty.
   *
   * Like Rust's Option::unwrap(): always checks, even in release builds.
   * For zero-cost access when the caller guarantees Some, use unwrapUnchecked().
   *
   * @return Reference to the value.
   */
  T &unwrap() & {
    XPP_ASSERT(m_hasValue, "unwrap() on None Option");
    return *reinterpret_cast<T *>(&m_storage);
  }

  const T &unwrap() const & {
    XPP_ASSERT(m_hasValue, "unwrap() on None Option");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  T &&unwrap() && {
    XPP_ASSERT(m_hasValue, "unwrap() on None Option");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get the held value without checking. UB if isNone().
   *
   * Like Rust's Option::unwrap_unchecked(). Debug builds assert; release
   * builds elide the check entirely. Caller must ensure isSome().
   *
   * @return Reference to the value.
   */
  T &unwrapUnchecked() & noexcept {
    XPP_DEBUG_ASSERT(m_hasValue, "internal: Option must be Some");
    return *reinterpret_cast<T *>(&m_storage);
  }

  const T &unwrapUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(m_hasValue, "internal: Option must be Some");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  T &&unwrapUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_hasValue, "internal: Option must be Some");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get the held value, or @p fallback if empty.
   * @param fallback  Value to return if empty.
   * @return          Reference to the value, or @p fallback.
   */
  const T &unwrapOr(const T &fallback) const & {
    return m_hasValue ? unwrapUnchecked() : fallback;
  }

  T unwrapOr(T &&fallback) && {
    return m_hasValue ? std::move(unwrapUnchecked()) : std::move(fallback);
  }

  /**
   * @brief Take the value out, leaving this Option empty.
   *
   * After this call isNone() is true. The returned Option owns the value.
   *
   * @return An Option containing the value, or none if this was empty.
   */
  Option take() {
    if (!m_hasValue) return none;
    Option r(std::move(unwrapUnchecked()));
    clear();
    return r;
  }

  /**
   * @brief Get the held value, aborting with @p msg if empty.
   *
   * Like Rust's Option::expect(). Useful when the failure message
   * should describe the invariant being violated, not the type.
   *
   * @param msg  Static C-string included in the panic output.
   * @return     Reference to the value.
   */
  T &expect(const char *msg) & {
    XPP_ASSERT(m_hasValue, "expect: %s", msg);
    return *reinterpret_cast<T *>(&m_storage);
  }
  const T &expect(const char *msg) const & {
    XPP_ASSERT(m_hasValue, "expect: %s", msg);
    return *reinterpret_cast<const T *>(&m_storage);
  }
  T &&expect(const char *msg) && {
    XPP_ASSERT(m_hasValue, "expect: %s", msg);
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Apply @p fn to the value, returning Option<U>. None passes through.
   *
   * Mirrors Rust's Option::map.
   */
  template <class Func>
  auto map(Func &&fn) const & -> Option<decltype(fn(std::declval<const T &>()))> {
    using U = decltype(fn(std::declval<const T &>()));
    return m_hasValue ? Option<U>(fn(unwrapUnchecked())) : Option<U>(none);
  }
  template <class Func> auto map(Func &&fn) && -> Option<decltype(fn(std::declval<T &&>()))> {
    using U = decltype(fn(std::declval<T &&>()));
    return m_hasValue ? Option<U>(fn(std::move(unwrapUnchecked()))) : Option<U>(none);
  }

  /**
   * @brief Monadic bind: apply @p fn to the value (fn returns Option<U>).
   *
   * Mirrors Rust's Option::and_then. fn must return some Option<U>; we
   * cannot statically constrain this without pulling in is_option.
   * Returns None unchanged.
   */
  template <class Func>
  auto andThen(Func &&fn) const & -> decltype(fn(std::declval<const T &>())) {
    using R = decltype(fn(std::declval<const T &>()));
    return m_hasValue ? fn(unwrapUnchecked()) : R(none);
  }
  template <class Func> auto andThen(Func &&fn) && -> decltype(fn(std::declval<T &&>())) {
    using R = decltype(fn(std::declval<T &&>()));
    return m_hasValue ? fn(std::move(unwrapUnchecked())) : R(none);
  }

  /**
   * @brief If None, call @p fn (returning Option<T>); otherwise pass through.
   *
   * Mirrors Rust's Option::or_else.
   */
  template <class Func> Option orElse(Func &&fn) const & {
    return m_hasValue ? *this : fn();
  }
  template <class Func> Option orElse(Func &&fn) && {
    return m_hasValue ? Option(std::move(*this)) : fn();
  }

  /**
   * @brief Get value if Some, else call @p fn for a fallback.
   *
   * Mirrors Rust's Option::unwrap_or_else. Consuming overload only.
   */
  template <class Func> T unwrapOrElse(Func &&fn) && {
    return m_hasValue ? std::move(unwrapUnchecked()) : fn();
  }

  /**
   * @brief Keep Some only if pred(value) is true; else None.
   *
   * Mirrors Rust's Option::filter. Consuming overload only — taking
   * Option by value matches Rust's `self` semantics.
   */
  template <class Func> Option filter(Func &&pred) && {
    if (m_hasValue && pred(unwrapUnchecked())) {
      return Option(std::move(unwrapUnchecked()));
    }
    return none;
  }

  /**
   * @brief Call @p fn(value) if Some; always return *this (chainable).
   *
   * Mirrors Rust's Option::inspect. fn is invoked for side effects
   * (logging, debugging) and its return value is discarded.
   */
  template <class Func> Option &inspect(Func &&fn) & {
    if (m_hasValue) fn(unwrapUnchecked());
    return *this;
  }
  template <class Func> const Option &inspect(Func &&fn) const & {
    if (m_hasValue) fn(unwrapUnchecked());
    return *this;
  }
  template <class Func> Option inspect(Func &&fn) && {
    if (m_hasValue) fn(unwrapUnchecked());
    return std::move(*this);
  }

  /**
   * @brief Convert to Result: Some(v) -> Ok(v), None -> Err(err).
   *
   * Mirrors Rust's Option::ok_or. Caller must have included result.h.
   * Consuming overload only.
   */
  template <class E> Result<T, E> okOr(E e) &&;

  /**
   * @brief Same as okOr but error is computed lazily by @p fn.
   *
   * Mirrors Rust's Option::ok_or_else. Caller must have included result.h.
   */
  template <class Func> auto okOrElse(Func &&fn) && -> Result<T, decltype(fn())>;

private:
  void clear() {
    if (m_hasValue) {
      reinterpret_cast<T *>(&m_storage)->~T();
      m_hasValue = false;
    }
  }

  bool                                                       m_hasValue = false;
  typename std::aligned_storage<sizeof(T), alignof(T)>::type m_storage;
};

/**
 * @brief Construct an Option with a value.
 *
 * Usage: auto o = Some(42);  // Option<int>
 */
template <class T> Option<typename std::decay<T>::type> Some(T &&val) {
  return Option<typename std::decay<T>::type>(std::forward<T>(val));
}

} // namespace xpp

#endif // XPP_OPTION_H
