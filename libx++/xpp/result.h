/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * result.h - Result<T, E>: a value or an error (like std::expected).
 *
 * No empty state — a Result is always Ok or Err.
 * Misuse (unwrap on Err, unwrap_err on Ok) panics via xbase/log.
 * C++11-compatible.
 */

#ifndef XPP_RESULT_H
#define XPP_RESULT_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/variant.h>

#include <utility>

namespace xpp {
namespace _ {

/** Trait: is_option<T>::value is true iff T is Option<U> for some U. */
template <class T> struct is_option : std::false_type {};
template <class U> struct is_option<Option<U>> : std::true_type {};

} // namespace _

/**
 * @brief Placeholder tag for the "success" variant.
 *
 * Reusable across types: Result, Option, etc.
 * Usage: Result<int, xErrno> r(ok, 42);
 */
struct Ok {
  explicit Ok() = default;
};

/**
 * @brief Placeholder tag for the "error" variant.
 *
 * Reusable across types: Result, Option, etc.
 * Usage: Result<int, xErrno> r(err, xErrno_NoMemory);
 */
struct Err {
  explicit Err() = default;
};

constexpr Ok  ok{};
constexpr Err err{};

/**
 * @brief A value or an error — like Rust's Result or C++23's std::expected.
 *
 * Always holds exactly one of: T (Ok) or E (Err). No empty/default state.
 *
 * @tparam T  Value type stored on success.
 * @tparam E  Error type stored on failure.
 */
template <class T, typename E> class Result {
public:
  /** Construct with Ok value. */
  Result(Ok, const T &val) : m_data(InPlaceIndex<0>{}, val) {}
  Result(Ok, T &&val) : m_data(InPlaceIndex<0>{}, std::move(val)) {}

  /** Construct with Err value. */
  Result(Err, const E &e) : m_data(InPlaceIndex<1>{}, e) {}
  Result(Err, E &&e) : m_data(InPlaceIndex<1>{}, std::move(e)) {}

  /** True if this holds an Ok value. */
  bool is_ok() const noexcept {
    return m_data.index() == 0;
  }

  /** True if this holds an Err value. */
  bool is_err() const noexcept {
    return m_data.index() == 1;
  }

  /**
   * @brief Get the Ok value, aborting if Err.
   *
   * Like Rust's Result::unwrap(): always checks, even in release builds.
   * For zero-cost access when the caller guarantees Ok, use unwrap_unchecked().
   *
   * @return Reference to the held value.
   */
  T &unwrap() & {
    XPP_ASSERT(is_ok(), "unwrap() on Err Result");
    return m_data.template get_unchecked<0>();
  }

  const T &unwrap() const & {
    XPP_ASSERT(is_ok(), "unwrap() on Err Result");
    return m_data.template get_unchecked<0>();
  }

  T &&unwrap() && {
    XPP_ASSERT(is_ok(), "unwrap() on Err Result");
    return std::move(m_data.template get_unchecked<0>());
  }

  /**
   * @brief Get the Ok value without checking. UB if is_err().
   *
   * Like Rust's Result::unwrap_unchecked(). Debug builds assert; release
   * builds elide the check. Caller must ensure is_ok().
   */
  T &unwrap_unchecked() & noexcept {
    XPP_DEBUG_ASSERT(is_ok(), "internal: Result must be Ok");
    return m_data.template get_unchecked<0>();
  }

  const T &unwrap_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(is_ok(), "internal: Result must be Ok");
    return m_data.template get_unchecked<0>();
  }

  T &&unwrap_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(is_ok(), "internal: Result must be Ok");
    return std::move(m_data.template get_unchecked<0>());
  }

  /**
   * @brief Get the Err value, aborting if Ok.
   *
   * Like Rust's Result::unwrap_err().
   */
  E &unwrap_err() & {
    XPP_ASSERT(is_err(), "unwrap_err() on Ok Result");
    return m_data.template get_unchecked<1>();
  }

  const E &unwrap_err() const & {
    XPP_ASSERT(is_err(), "unwrap_err() on Ok Result");
    return m_data.template get_unchecked<1>();
  }

  E &&unwrap_err() && {
    XPP_ASSERT(is_err(), "unwrap_err() on Ok Result");
    return std::move(m_data.template get_unchecked<1>());
  }

  /**
   * @brief Get the Err value without checking. UB if is_ok().
   *
   * Like Rust's Result::unwrap_err_unchecked().
   */
  E &unwrap_err_unchecked() & noexcept {
    XPP_DEBUG_ASSERT(is_err(), "internal: Result must be Err");
    return m_data.template get_unchecked<1>();
  }

  const E &unwrap_err_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(is_err(), "internal: Result must be Err");
    return m_data.template get_unchecked<1>();
  }

  E &&unwrap_err_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(is_err(), "internal: Result must be Err");
    return std::move(m_data.template get_unchecked<1>());
  }

  /**
   * @brief Consume this Result; return Some(value) if Ok, None if Err.
   *
   * Mirrors Rust's Result::ok(). Discards the error on Err. Must be
   * called on an rvalue:
   *
   *   auto opt = std::move(r).ok();
   *
   * After this call @p r is moved-from; do not access it again except
   * to destroy it.
   *
   * @return Option<T> containing the value, or None.
   */
  Option<T> ok() && {
    return is_ok() ? Option<T>(std::move(m_data.template get_unchecked<0>())) : Option<T>(none);
  }

  /**
   * @brief Consume this Result; return Some(error) if Err, None if Ok.
   *
   * Mirrors Rust's Result::err(). Discards the value on Ok. See ok()
   * for usage notes.
   *
   * @return Option<E> containing the error, or None.
   */
  Option<E> err() && {
    return is_err() ? Option<E>(std::move(m_data.template get_unchecked<1>())) : Option<E>(none);
  }

  /**
   * @brief Get the Ok value, or @p fallback if Err.
   * @param fallback  Value to return if Err.
   * @return          Reference to the value, or @p fallback.
   */
  const T &unwrap_or(const T &fallback) const & {
    return is_ok() ? unwrap_unchecked() : fallback;
  }

  T unwrap_or(T &&fallback) && {
    return is_ok() ? std::move(unwrap_unchecked()) : std::move(fallback);
  }

  /** Dereference: returns the Ok value. UB if is_err(). */
  T &operator*() & {
    return unwrap_unchecked();
  }
  const T &operator*() const & {
    return unwrap_unchecked();
  }
  T &&operator*() && {
    return std::move(unwrap_unchecked());
  }

  /** Arrow access to the Ok value. UB if is_err(). */
  T *operator->() {
    return &unwrap_unchecked();
  }
  const T *operator->() const {
    return &unwrap_unchecked();
  }

  /** Bool conversion: true if Ok. */
  explicit operator bool() const noexcept {
    return is_ok();
  }

  /**
   * @brief Apply @p fn to the Ok value; propagate Err unchanged.
   *
   * Result<int, E> r(ok, 42);
   * auto s = r.map([](int x) { return x + 1; });  // Result<int, E>(ok, 43)
   */
  template <class Func>
  auto map(Func &&fn) const & -> Result<decltype(fn(std::declval<const T &>())), E> {
    using U = decltype(fn(std::declval<const T &>()));
    return is_ok() ? Result<U, E>(xpp::ok, fn(unwrap_unchecked()))
                  : Result<U, E>(xpp::err, unwrap_err_unchecked());
  }

  template <class Func> auto map(Func &&fn) && -> Result<decltype(fn(std::declval<T &&>())), E> {
    using U = decltype(fn(std::declval<T &&>()));
    return is_ok() ? Result<U, E>(xpp::ok, fn(std::move(unwrap_unchecked())))
                  : Result<U, E>(xpp::err, std::move(unwrap_err_unchecked()));
  }

  /**
   * @brief Swap the layering of Result<Option<U>, E>.
   *
   * Mirrors Rust's Result::transpose. Only callable when T is some
   * Option<U>; SFINAE removes this overload otherwise.
   *
   *   Ok(Some(x)) -> Some(Ok(x))
   *   Ok(None)    -> None
   *   Err(e)      -> Some(Err(e))
   *
   * Consumes *this. Use as: auto out = std::move(r).transpose();
   *
   * @return Option<Result<U, E>> per the mapping above.
   */
  template <class U = T, typename = typename std::enable_if<_::is_option<U>::value>::type>
  Option<Result<typename U::value_type, E>> transpose() && {
    using Inner = typename U::value_type;
    if (is_err()) {
      return Some(Result<Inner, E>(xpp::err, std::move(*this).unwrap_err()));
    }
    Option<Inner> inner = std::move(*this).unwrap();
    if (inner.is_none()) return none;
    return Some(Result<Inner, E>(xpp::ok, std::move(inner).unwrap()));
  }

  /**
   * @brief Get the Ok value, aborting with @p msg if Err.
   *
   * Like Rust's Result::expect.
   */
  T &expect(const char *msg) & {
    XPP_ASSERT(is_ok(), "expect: %s", msg);
    return m_data.template get_unchecked<0>();
  }
  const T &expect(const char *msg) const & {
    XPP_ASSERT(is_ok(), "expect: %s", msg);
    return m_data.template get_unchecked<0>();
  }
  T &&expect(const char *msg) && {
    XPP_ASSERT(is_ok(), "expect: %s", msg);
    return std::move(m_data.template get_unchecked<0>());
  }

  /**
   * @brief Get the Err value, aborting with @p msg if Ok.
   *
   * Like Rust's Result::expect_err.
   */
  E &expect_err(const char *msg) & {
    XPP_ASSERT(is_err(), "expect_err: %s", msg);
    return m_data.template get_unchecked<1>();
  }
  const E &expect_err(const char *msg) const & {
    XPP_ASSERT(is_err(), "expect_err: %s", msg);
    return m_data.template get_unchecked<1>();
  }
  E &&expect_err(const char *msg) && {
    XPP_ASSERT(is_err(), "expect_err: %s", msg);
    return std::move(m_data.template get_unchecked<1>());
  }

  /**
   * @brief Apply @p fn to the Err value; propagate Ok unchanged.
   *
   * Mirrors Rust's Result::map_err.
   */
  template <class Func>
  auto map_err(Func &&fn) const & -> Result<T, decltype(fn(std::declval<const E &>()))> {
    using F = decltype(fn(std::declval<const E &>()));
    return is_ok() ? Result<T, F>(xpp::ok, unwrap_unchecked())
                  : Result<T, F>(xpp::err, fn(unwrap_err_unchecked()));
  }
  template <class Func>
  auto map_err(Func &&fn) && -> Result<T, decltype(fn(std::declval<E &&>()))> {
    using F = decltype(fn(std::declval<E &&>()));
    return is_ok() ? Result<T, F>(xpp::ok, std::move(unwrap_unchecked()))
                  : Result<T, F>(xpp::err, fn(std::move(unwrap_err_unchecked())));
  }

  /**
   * @brief Monadic bind: apply @p fn(value) on Ok; pass through on Err.
   *
   * Mirrors Rust's Result::and_then. fn must return some Result<U, E>.
   */
  template <class Func>
  auto and_then(Func &&fn) const & -> decltype(fn(std::declval<const T &>())) {
    using R = decltype(fn(std::declval<const T &>()));
    return is_ok() ? fn(unwrap_unchecked()) : R(xpp::err, unwrap_err_unchecked());
  }
  template <class Func> auto and_then(Func &&fn) && -> decltype(fn(std::declval<T &&>())) {
    using R = decltype(fn(std::declval<T &&>()));
    return is_ok() ? fn(std::move(unwrap_unchecked())) : R(xpp::err, std::move(unwrap_err_unchecked()));
  }

  /**
   * @brief If Err, call @p fn(err) (returns Result<T, F>); else pass through.
   *
   * Mirrors Rust's Result::or_else.
   */
  template <class Func>
  auto or_else(Func &&fn) const & -> decltype(fn(std::declval<const E &>())) {
    using R = decltype(fn(std::declval<const E &>()));
    return is_ok() ? R(xpp::ok, unwrap_unchecked()) : fn(unwrap_err_unchecked());
  }
  template <class Func> auto or_else(Func &&fn) && -> decltype(fn(std::declval<E &&>())) {
    using R = decltype(fn(std::declval<E &&>()));
    return is_ok() ? R(xpp::ok, std::move(unwrap_unchecked())) : fn(std::move(unwrap_err_unchecked()));
  }

  /**
   * @brief Get Ok value, else call @p fn(err) for a fallback.
   *
   * Mirrors Rust's Result::unwrap_or_else. Consuming overload only.
   */
  template <class Func> T unwrap_or_else(Func &&fn) && {
    return is_ok() ? std::move(unwrap_unchecked()) : fn(std::move(unwrap_err_unchecked()));
  }

  /**
   * @brief Call @p fn(value) if Ok; chainable, returns *this.
   *
   * Mirrors Rust's Result::inspect.
   */
  template <class Func> Result &inspect(Func &&fn) & {
    if (is_ok()) fn(unwrap_unchecked());
    return *this;
  }
  template <class Func> const Result &inspect(Func &&fn) const & {
    if (is_ok()) fn(unwrap_unchecked());
    return *this;
  }
  template <class Func> Result inspect(Func &&fn) && {
    if (is_ok()) fn(unwrap_unchecked());
    return std::move(*this);
  }

  /**
   * @brief Call @p fn(err) if Err; chainable, returns *this.
   *
   * Mirrors Rust's Result::inspect_err.
   */
  template <class Func> Result &inspect_err(Func &&fn) & {
    if (is_err()) fn(unwrap_err_unchecked());
    return *this;
  }
  template <class Func> const Result &inspect_err(Func &&fn) const & {
    if (is_err()) fn(unwrap_err_unchecked());
    return *this;
  }
  template <class Func> Result inspect_err(Func &&fn) && {
    if (is_err()) fn(unwrap_err_unchecked());
    return std::move(*this);
  }

private:
  Variant<T, E> m_data;
};

/**
 * @brief Specialization for void Ok type (operation that can fail with no value).
 */
template <class E> class Result<void, E> {
  struct OkSentinel {}; // zero-size tag

public:
  /** Construct a successful (void) result. */
  Result(Ok) : m_data(OkSentinel{}) {}

  /** Construct with Err value. */
  Result(Err, const E &e) : m_data(e) {}
  Result(Err, E &&e) : m_data(std::move(e)) {}

  bool is_ok() const noexcept {
    return m_data.template is<OkSentinel>();
  }
  bool is_err() const noexcept {
    return m_data.template is<E>();
  }

  /**
   * @brief Get the Err value, aborting if Ok.
   * @return Reference to the held error.
   */
  E &unwrap_err() & {
    XPP_ASSERT(is_err(), "unwrap_err() on Ok Result");
    return m_data.template get_unchecked<1>();
  }

  const E &unwrap_err() const & {
    XPP_ASSERT(is_err(), "unwrap_err() on Ok Result");
    return m_data.template get_unchecked<1>();
  }

  E &&unwrap_err() && {
    XPP_ASSERT(is_err(), "unwrap_err() on Ok Result");
    return std::move(m_data.template get_unchecked<1>());
  }

  /**
   * @brief Get the Err value without checking. UB if is_ok().
   */
  E &unwrap_err_unchecked() & noexcept {
    XPP_DEBUG_ASSERT(is_err(), "internal: Result must be Err");
    return m_data.template get_unchecked<1>();
  }

  const E &unwrap_err_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(is_err(), "internal: Result must be Err");
    return m_data.template get_unchecked<1>();
  }

  E &&unwrap_err_unchecked() && noexcept {
    XPP_DEBUG_ASSERT(is_err(), "internal: Result must be Err");
    return std::move(m_data.template get_unchecked<1>());
  }

  /**
   * @brief Consume this Result; return Some(error) if Err, None if Ok.
   *
   * Mirrors Rust's Result::err(). Must be called on an rvalue:
   *
   *   auto opt = std::move(r).err();
   *
   * After this call @p r is moved-from; do not access it again except
   * to destroy it.
   *
   * @return Option<E> containing the error, or None.
   */
  Option<E> err() && {
    return is_err() ? Option<E>(std::move(m_data.template get_unchecked<1>())) : Option<E>(none);
  }

  explicit operator bool() const noexcept {
    return is_ok();
  }

  /** Like Rust's Result::expect_err. */
  E &expect_err(const char *msg) & {
    XPP_ASSERT(is_err(), "expect_err: %s", msg);
    return m_data.template get_unchecked<1>();
  }
  const E &expect_err(const char *msg) const & {
    XPP_ASSERT(is_err(), "expect_err: %s", msg);
    return m_data.template get_unchecked<1>();
  }
  E &&expect_err(const char *msg) && {
    XPP_ASSERT(is_err(), "expect_err: %s", msg);
    return std::move(m_data.template get_unchecked<1>());
  }

  /** Map E -> F. */
  template <class Func>
  auto map_err(Func &&fn) const & -> Result<void, decltype(fn(std::declval<const E &>()))> {
    using F = decltype(fn(std::declval<const E &>()));
    return is_ok() ? Result<void, F>(xpp::ok) : Result<void, F>(xpp::err, fn(unwrap_err_unchecked()));
  }
  template <class Func>
  auto map_err(Func &&fn) && -> Result<void, decltype(fn(std::declval<E &&>()))> {
    using F = decltype(fn(std::declval<E &&>()));
    return is_ok() ? Result<void, F>(xpp::ok)
                  : Result<void, F>(xpp::err, fn(std::move(unwrap_err_unchecked())));
  }

  /** Monadic bind: fn takes no args and returns Result<U, E>. */
  template <class Func> auto and_then(Func &&fn) const & -> decltype(fn()) {
    using R = decltype(fn());
    return is_ok() ? fn() : R(xpp::err, unwrap_err_unchecked());
  }
  template <class Func> auto and_then(Func &&fn) && -> decltype(fn()) {
    using R = decltype(fn());
    return is_ok() ? fn() : R(xpp::err, std::move(unwrap_err_unchecked()));
  }

  /** If Err, recover via fn(err) -> Result<void, F>. */
  template <class Func>
  auto or_else(Func &&fn) const & -> decltype(fn(std::declval<const E &>())) {
    using R = decltype(fn(std::declval<const E &>()));
    return is_ok() ? R(xpp::ok) : fn(unwrap_err_unchecked());
  }
  template <class Func> auto or_else(Func &&fn) && -> decltype(fn(std::declval<E &&>())) {
    using R = decltype(fn(std::declval<E &&>()));
    return is_ok() ? R(xpp::ok) : fn(std::move(unwrap_err_unchecked()));
  }

  /** Chainable side-effect on Err. */
  template <class Func> Result &inspect_err(Func &&fn) & {
    if (is_err()) fn(unwrap_err_unchecked());
    return *this;
  }
  template <class Func> const Result &inspect_err(Func &&fn) const & {
    if (is_err()) fn(unwrap_err_unchecked());
    return *this;
  }
  template <class Func> Result inspect_err(Func &&fn) && {
    if (is_err()) fn(unwrap_err_unchecked());
    return std::move(*this);
  }

private:
  Variant<OkSentinel, E> m_data;
};

/* ── Option::ok_or / ok_or_else: out-of-line because they depend on Result. ── */

template <class T> template <class E> Result<T, E> Option<T>::ok_or(E e) && {
  return m_has_value ? Result<T, E>(xpp::ok, std::move(unwrap_unchecked()))
                    : Result<T, E>(xpp::err, std::move(e));
}

template <class T>
template <class Func>
auto Option<T>::ok_or_else(Func &&fn) && -> Result<T, decltype(fn())> {
  using E = decltype(fn());
  return m_has_value ? Result<T, E>(xpp::ok, std::move(unwrap_unchecked()))
                    : Result<T, E>(xpp::err, fn());
}

} // namespace xpp

#endif // XPP_RESULT_H
