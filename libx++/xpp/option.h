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

#include <new>  // IWYU pragma: keep  (placement new: `new (ptr) T(...)`)
#include <utility>

namespace xpp {

/** Forward declaration so Option::ok_or / ok_or_else can name Result. */
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
  constexpr Option() noexcept : m_has_value(false) {}

  /** @brief Construct an empty Option from none. */
  constexpr Option(None) noexcept : m_has_value(false) {}

  /** @brief Construct with a value. */
  Option(const T &val) : m_has_value(true) {
    new (&m_storage) T(val);
  }

  Option(T &&val) noexcept : m_has_value(true) {
    new (&m_storage) T(std::move(val));
  }

  /** @brief Copy constructor. */
  Option(const Option &o) : m_has_value(o.m_has_value) {
    if (m_has_value) new (&m_storage) T(o.unwrap());
  }

  /** @brief Move constructor. Source is left empty. */
  Option(Option &&o) noexcept : m_has_value(o.m_has_value) {
    if (m_has_value) {
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
      m_has_value = o.m_has_value;
      if (m_has_value) new (&m_storage) T(o.unwrap());
    }
    return *this;
  }

  /** @brief Move assignment. Source is left empty. */
  Option &operator=(Option &&o) noexcept {
    if (this != &o) {
      clear();
      m_has_value = o.m_has_value;
      if (m_has_value) {
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
  bool is_some() const noexcept {
    return m_has_value;
  }

  /** @brief True if this is empty. */
  bool is_none() const noexcept {
    return !m_has_value;
  }

  /** @brief Bool conversion: true if some. */
  explicit operator bool() const noexcept {
    return m_has_value;
  }

  /**
   * @brief Get the held value, aborting if empty.
   *
   * Like Rust's Option::unwrap(): always checks, even in release builds.
   * For zero-cost access when the caller guarantees Some, use unwrap_unchecked().
   *
   * @return Reference to the value.
   */
  T &unwrap() & {
    XPP_ASSERT(m_has_value, "unwrap() on None Option");
    return *reinterpret_cast<T *>(&m_storage);
  }

  const T &unwrap() const & {
    XPP_ASSERT(m_has_value, "unwrap() on None Option");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  T &&unwrap() && {
    XPP_ASSERT(m_has_value, "unwrap() on None Option");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get the held value without checking. UB if is_none().
   *
   * Like Rust's Option::unwrap_unchecked(). Debug builds assert; release
   * builds elide the check entirely. Caller must ensure is_some().
   *
   * @return Reference to the value.
   */
  T &unwrap_unchecked() & noexcept {
    XPP_DEBUG_ASSERT(m_has_value, "internal: Option must be Some");
    return *reinterpret_cast<T *>(&m_storage);
  }

  const T &unwrap_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(m_has_value, "internal: Option must be Some");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  T &&unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(m_has_value, "internal: Option must be Some");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get the held value, or @p fallback if empty.
   * @param fallback  Value to return if empty.
   * @return          Reference to the value, or @p fallback.
   */
  const T &unwrap_or(const T &fallback) const & {
    return m_has_value ? unwrap_unchecked() : fallback;
  }

  T unwrap_or(T &&fallback) && {
    return m_has_value ? std::move(unwrap_unchecked()) : std::move(fallback);
  }

  /**
   * @brief Take the value out, leaving this Option empty.
   *
   * After this call is_none() is true. The returned Option owns the value.
   *
   * @return An Option containing the value, or none if this was empty.
   */
  Option take() {
    if (!m_has_value) return none;
    Option r(std::move(unwrap_unchecked()));
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
    XPP_ASSERT(m_has_value, "expect: %s", msg);
    return *reinterpret_cast<T *>(&m_storage);
  }
  const T &expect(const char *msg) const & {
    XPP_ASSERT(m_has_value, "expect: %s", msg);
    return *reinterpret_cast<const T *>(&m_storage);
  }
  T &&expect(const char *msg) && {
    XPP_ASSERT(m_has_value, "expect: %s", msg);
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
    return m_has_value ? Option<U>(fn(unwrap_unchecked())) : Option<U>(none);
  }
  template <class Func> auto map(Func &&fn) && -> Option<decltype(fn(std::declval<T &&>()))> {
    using U = decltype(fn(std::declval<T &&>()));
    return m_has_value ? Option<U>(fn(std::move(unwrap_unchecked()))) : Option<U>(none);
  }

  /**
   * @brief Monadic bind: apply @p fn to the value (fn returns Option<U>).
   *
   * Mirrors Rust's Option::and_then. fn must return some Option<U>; we
   * cannot statically constrain this without pulling in is_option.
   * Returns None unchanged.
   */
  template <class Func>
  auto and_then(Func &&fn) const & -> decltype(fn(std::declval<const T &>())) {
    using R = decltype(fn(std::declval<const T &>()));
    return m_has_value ? fn(unwrap_unchecked()) : R(none);
  }
  template <class Func> auto and_then(Func &&fn) && -> decltype(fn(std::declval<T &&>())) {
    using R = decltype(fn(std::declval<T &&>()));
    return m_has_value ? fn(std::move(unwrap_unchecked())) : R(none);
  }

  /**
   * @brief If None, call @p fn (returning Option<T>); otherwise pass through.
   *
   * Mirrors Rust's Option::or_else.
   */
  template <class Func> Option or_else(Func &&fn) const & {
    return m_has_value ? *this : fn();
  }
  template <class Func> Option or_else(Func &&fn) && {
    return m_has_value ? Option(std::move(*this)) : fn();
  }

  /**
   * @brief Get value if Some, else call @p fn for a fallback.
   *
   * Mirrors Rust's Option::unwrap_or_else. Consuming overload only.
   */
  template <class Func> T unwrap_or_else(Func &&fn) && {
    return m_has_value ? std::move(unwrap_unchecked()) : fn();
  }

  /**
   * @brief Keep Some only if pred(value) is true; else None.
   *
   * Mirrors Rust's Option::filter. Consuming overload only — taking
   * Option by value matches Rust's `self` semantics.
   */
  template <class Func> Option filter(Func &&pred) && {
    if (m_has_value && pred(unwrap_unchecked())) {
      return Option(std::move(unwrap_unchecked()));
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
    if (m_has_value) fn(unwrap_unchecked());
    return *this;
  }
  template <class Func> const Option &inspect(Func &&fn) const & {
    if (m_has_value) fn(unwrap_unchecked());
    return *this;
  }
  template <class Func> Option inspect(Func &&fn) && {
    if (m_has_value) fn(unwrap_unchecked());
    return std::move(*this);
  }

  /**
   * @brief Convert to Result: Some(v) -> Ok(v), None -> Err(err).
   *
   * Mirrors Rust's Option::ok_or. Caller must have included result.h.
   * Consuming overload only.
   */
  template <class E> Result<T, E> ok_or(E e) &&;

  /**
   * @brief Same as ok_or but error is computed lazily by @p fn.
   *
   * Mirrors Rust's Option::ok_or_else. Caller must have included result.h.
   */
  template <class Func> auto ok_or_else(Func &&fn) && -> Result<T, decltype(fn())>;

private:
  void clear() {
    if (m_has_value) {
      reinterpret_cast<T *>(&m_storage)->~T();
      m_has_value = false;
    }
  }

  bool                                                       m_has_value = false;
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

/* ── Option<T&> specialization ─────────────────────────────────────── */

/**
 * @brief Option holding a reference — a safe nullable reference.
 *
 * Zero-overhead: sizeof(Option<T&>) == sizeof(T*).
 * Replaces raw pointers that express "might be null."
 *
 * @code
 *   int x = 42;
 *   Option<int&> ref(x);
 *   ref.unwrap() = 10;  // modifies x
 *
 *   Option<int&> empty(none);
 *   // empty.unwrap();  // panics
 * @endcode
 */
template <class T> class Option<T &> {
public:
  using value_type = T &;

  constexpr Option() noexcept : m_ptr(nullptr) {}
  constexpr Option(None) noexcept : m_ptr(nullptr) {}
  Option(T &ref) noexcept : m_ptr(&ref) {}

  /* Rebindable (unlike actual C++ references). */
  Option(const Option &) noexcept = default;
  Option &operator=(const Option &) noexcept = default;

  Option &operator=(None) noexcept {
    m_ptr = nullptr;
    return *this;
  }

  bool is_some() const noexcept { return m_ptr != nullptr; }
  bool is_none() const noexcept { return m_ptr == nullptr; }
  explicit operator bool() const noexcept { return m_ptr != nullptr; }

  T &unwrap() const {
    XPP_ASSERT(m_ptr != nullptr, "unwrap() on None Option<T&>");
    return *m_ptr;
  }

  T &unwrap_unchecked() const noexcept {
    XPP_DEBUG_ASSERT(m_ptr != nullptr, "internal: Option<T&> must be Some");
    return *m_ptr;
  }

  T &unwrap_or(T &fallback) const noexcept {
    return m_ptr ? *m_ptr : fallback;
  }

  T &expect(const char *msg) const {
    XPP_ASSERT(m_ptr != nullptr, "expect: %s", msg);
    return *m_ptr;
  }

  /**
   * @brief Take the reference out, leaving this Option empty.
   */
  Option take() noexcept {
    Option r(*this);
    m_ptr = nullptr;
    return r;
  }

  template <class Func>
  auto map(Func &&fn) const -> Option<decltype(fn(std::declval<T &>()))> {
    using U = decltype(fn(std::declval<T &>()));
    return m_ptr ? Option<U>(fn(*m_ptr)) : Option<U>(none);
  }

  template <class Func>
  auto and_then(Func &&fn) const -> decltype(fn(std::declval<T &>())) {
    using R = decltype(fn(std::declval<T &>()));
    return m_ptr ? fn(*m_ptr) : R(none);
  }

  template <class Func> Option or_else(Func &&fn) const {
    return m_ptr ? *this : fn();
  }

  template <class Func> Option filter(Func &&pred) const {
    return (m_ptr && pred(*m_ptr)) ? *this : Option(none);
  }

  template <class Func> const Option &inspect(Func &&fn) const {
    if (m_ptr) fn(*m_ptr);
    return *this;
  }

private:
  T *m_ptr;
};

} // namespace xpp

#endif // XPP_OPTION_H
