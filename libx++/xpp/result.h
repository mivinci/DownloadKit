/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * result.h - Result<T, E>: a value or an error (like std::expected).
 *
 * No empty state — a Result is always Ok or Err.
 * Misuse (unwrap on Err, unwrapErr on Ok) panics via xbase/log.
 * C++14-compatible.
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
  bool isOk() const noexcept {
    return m_data.index() == 0;
  }

  /** True if this holds an Err value. */
  bool isErr() const noexcept {
    return m_data.index() == 1;
  }

  /**
   * @brief Get the Ok value, aborting if Err.
   *
   * Like Rust's Result::unwrap(): always checks, even in release builds.
   * For zero-cost access when the caller guarantees Ok, use unwrapUnchecked().
   *
   * @return Reference to the held value.
   */
  T &unwrap() & {
    XPP_ASSERT(isOk(), "unwrap() on Err Result");
    return m_data.template getUnchecked<0>();
  }

  const T &unwrap() const & {
    XPP_ASSERT(isOk(), "unwrap() on Err Result");
    return m_data.template getUnchecked<0>();
  }

  T &&unwrap() && {
    XPP_ASSERT(isOk(), "unwrap() on Err Result");
    return std::move(m_data.template getUnchecked<0>());
  }

  /**
   * @brief Get the Ok value without checking. UB if isErr().
   *
   * Like Rust's Result::unwrap_unchecked(). Debug builds assert; release
   * builds elide the check. Caller must ensure isOk().
   */
  T &unwrapUnchecked() & noexcept {
    XPP_DEBUG_ASSERT(isOk(), "internal: Result must be Ok");
    return m_data.template getUnchecked<0>();
  }

  const T &unwrapUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(isOk(), "internal: Result must be Ok");
    return m_data.template getUnchecked<0>();
  }

  T &&unwrapUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(isOk(), "internal: Result must be Ok");
    return std::move(m_data.template getUnchecked<0>());
  }

  /**
   * @brief Get the Err value, aborting if Ok.
   *
   * Like Rust's Result::unwrap_err().
   */
  E &unwrapErr() & {
    XPP_ASSERT(isErr(), "unwrapErr() on Ok Result");
    return m_data.template getUnchecked<1>();
  }

  const E &unwrapErr() const & {
    XPP_ASSERT(isErr(), "unwrapErr() on Ok Result");
    return m_data.template getUnchecked<1>();
  }

  E &&unwrapErr() && {
    XPP_ASSERT(isErr(), "unwrapErr() on Ok Result");
    return std::move(m_data.template getUnchecked<1>());
  }

  /**
   * @brief Get the Err value without checking. UB if isOk().
   *
   * Like Rust's Result::unwrap_err_unchecked().
   */
  E &unwrapErrUnchecked() & noexcept {
    XPP_DEBUG_ASSERT(isErr(), "internal: Result must be Err");
    return m_data.template getUnchecked<1>();
  }

  const E &unwrapErrUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(isErr(), "internal: Result must be Err");
    return m_data.template getUnchecked<1>();
  }

  E &&unwrapErrUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(isErr(), "internal: Result must be Err");
    return std::move(m_data.template getUnchecked<1>());
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
    return isOk() ? Option<T>(std::move(m_data.template getUnchecked<0>())) : Option<T>(none);
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
    return isErr() ? Option<E>(std::move(m_data.template getUnchecked<1>())) : Option<E>(none);
  }

  /**
   * @brief Get the Ok value, or @p fallback if Err.
   * @param fallback  Value to return if Err.
   * @return          Reference to the value, or @p fallback.
   */
  const T &unwrapOr(const T &fallback) const & {
    return isOk() ? unwrapUnchecked() : fallback;
  }

  T unwrapOr(T &&fallback) && {
    return isOk() ? std::move(unwrapUnchecked()) : std::move(fallback);
  }

  /** Dereference: returns the Ok value. UB if isErr(). */
  T &operator*() & {
    return unwrapUnchecked();
  }
  const T &operator*() const & {
    return unwrapUnchecked();
  }
  T &&operator*() && {
    return std::move(unwrapUnchecked());
  }

  /** Arrow access to the Ok value. UB if isErr(). */
  T *operator->() {
    return &unwrapUnchecked();
  }
  const T *operator->() const {
    return &unwrapUnchecked();
  }

  /** Bool conversion: true if Ok. */
  explicit operator bool() const noexcept {
    return isOk();
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
    return isOk() ? Result<U, E>(xpp::ok, fn(unwrapUnchecked()))
                  : Result<U, E>(xpp::err, unwrapErrUnchecked());
  }

  template <class Func> auto map(Func &&fn) && -> Result<decltype(fn(std::declval<T &&>())), E> {
    using U = decltype(fn(std::declval<T &&>()));
    return isOk() ? Result<U, E>(xpp::ok, fn(std::move(unwrapUnchecked())))
                  : Result<U, E>(xpp::err, std::move(unwrapErrUnchecked()));
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
    if (isErr()) {
      return Some(Result<Inner, E>(xpp::err, std::move(*this).unwrapErr()));
    }
    Option<Inner> inner = std::move(*this).unwrap();
    if (inner.isNone()) return none;
    return Some(Result<Inner, E>(xpp::ok, std::move(inner).unwrap()));
  }

  /**
   * @brief Get the Ok value, aborting with @p msg if Err.
   *
   * Like Rust's Result::expect.
   */
  T &expect(const char *msg) & {
    XPP_ASSERT(isOk(), "expect: %s", msg);
    return m_data.template getUnchecked<0>();
  }
  const T &expect(const char *msg) const & {
    XPP_ASSERT(isOk(), "expect: %s", msg);
    return m_data.template getUnchecked<0>();
  }
  T &&expect(const char *msg) && {
    XPP_ASSERT(isOk(), "expect: %s", msg);
    return std::move(m_data.template getUnchecked<0>());
  }

  /**
   * @brief Get the Err value, aborting with @p msg if Ok.
   *
   * Like Rust's Result::expect_err.
   */
  E &expectErr(const char *msg) & {
    XPP_ASSERT(isErr(), "expectErr: %s", msg);
    return m_data.template getUnchecked<1>();
  }
  const E &expectErr(const char *msg) const & {
    XPP_ASSERT(isErr(), "expectErr: %s", msg);
    return m_data.template getUnchecked<1>();
  }
  E &&expectErr(const char *msg) && {
    XPP_ASSERT(isErr(), "expectErr: %s", msg);
    return std::move(m_data.template getUnchecked<1>());
  }

  /**
   * @brief Apply @p fn to the Err value; propagate Ok unchanged.
   *
   * Mirrors Rust's Result::map_err.
   */
  template <class Func>
  auto mapErr(Func &&fn) const & -> Result<T, decltype(fn(std::declval<const E &>()))> {
    using F = decltype(fn(std::declval<const E &>()));
    return isOk() ? Result<T, F>(xpp::ok, unwrapUnchecked())
                  : Result<T, F>(xpp::err, fn(unwrapErrUnchecked()));
  }
  template <class Func>
  auto mapErr(Func &&fn) && -> Result<T, decltype(fn(std::declval<E &&>()))> {
    using F = decltype(fn(std::declval<E &&>()));
    return isOk() ? Result<T, F>(xpp::ok, std::move(unwrapUnchecked()))
                  : Result<T, F>(xpp::err, fn(std::move(unwrapErrUnchecked())));
  }

  /**
   * @brief Monadic bind: apply @p fn(value) on Ok; pass through on Err.
   *
   * Mirrors Rust's Result::and_then. fn must return some Result<U, E>.
   */
  template <class Func>
  auto andThen(Func &&fn) const & -> decltype(fn(std::declval<const T &>())) {
    using R = decltype(fn(std::declval<const T &>()));
    return isOk() ? fn(unwrapUnchecked()) : R(xpp::err, unwrapErrUnchecked());
  }
  template <class Func> auto andThen(Func &&fn) && -> decltype(fn(std::declval<T &&>())) {
    using R = decltype(fn(std::declval<T &&>()));
    return isOk() ? fn(std::move(unwrapUnchecked())) : R(xpp::err, std::move(unwrapErrUnchecked()));
  }

  /**
   * @brief If Err, call @p fn(err) (returns Result<T, F>); else pass through.
   *
   * Mirrors Rust's Result::or_else.
   */
  template <class Func>
  auto orElse(Func &&fn) const & -> decltype(fn(std::declval<const E &>())) {
    using R = decltype(fn(std::declval<const E &>()));
    return isOk() ? R(xpp::ok, unwrapUnchecked()) : fn(unwrapErrUnchecked());
  }
  template <class Func> auto orElse(Func &&fn) && -> decltype(fn(std::declval<E &&>())) {
    using R = decltype(fn(std::declval<E &&>()));
    return isOk() ? R(xpp::ok, std::move(unwrapUnchecked())) : fn(std::move(unwrapErrUnchecked()));
  }

  /**
   * @brief Get Ok value, else call @p fn(err) for a fallback.
   *
   * Mirrors Rust's Result::unwrap_or_else. Consuming overload only.
   */
  template <class Func> T unwrapOrElse(Func &&fn) && {
    return isOk() ? std::move(unwrapUnchecked()) : fn(std::move(unwrapErrUnchecked()));
  }

  /**
   * @brief Call @p fn(value) if Ok; chainable, returns *this.
   *
   * Mirrors Rust's Result::inspect.
   */
  template <class Func> Result &inspect(Func &&fn) & {
    if (isOk()) fn(unwrapUnchecked());
    return *this;
  }
  template <class Func> const Result &inspect(Func &&fn) const & {
    if (isOk()) fn(unwrapUnchecked());
    return *this;
  }
  template <class Func> Result inspect(Func &&fn) && {
    if (isOk()) fn(unwrapUnchecked());
    return std::move(*this);
  }

  /**
   * @brief Call @p fn(err) if Err; chainable, returns *this.
   *
   * Mirrors Rust's Result::inspect_err.
   */
  template <class Func> Result &inspectErr(Func &&fn) & {
    if (isErr()) fn(unwrapErrUnchecked());
    return *this;
  }
  template <class Func> const Result &inspectErr(Func &&fn) const & {
    if (isErr()) fn(unwrapErrUnchecked());
    return *this;
  }
  template <class Func> Result inspectErr(Func &&fn) && {
    if (isErr()) fn(unwrapErrUnchecked());
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

  bool isOk() const noexcept {
    return m_data.template is<OkSentinel>();
  }
  bool isErr() const noexcept {
    return m_data.template is<E>();
  }

  /**
   * @brief Get the Err value, aborting if Ok.
   * @return Reference to the held error.
   */
  E &unwrapErr() & {
    XPP_ASSERT(isErr(), "unwrapErr() on Ok Result");
    return m_data.template getUnchecked<1>();
  }

  const E &unwrapErr() const & {
    XPP_ASSERT(isErr(), "unwrapErr() on Ok Result");
    return m_data.template getUnchecked<1>();
  }

  E &&unwrapErr() && {
    XPP_ASSERT(isErr(), "unwrapErr() on Ok Result");
    return std::move(m_data.template getUnchecked<1>());
  }

  /**
   * @brief Get the Err value without checking. UB if isOk().
   */
  E &unwrapErrUnchecked() & noexcept {
    XPP_DEBUG_ASSERT(isErr(), "internal: Result must be Err");
    return m_data.template getUnchecked<1>();
  }

  const E &unwrapErrUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(isErr(), "internal: Result must be Err");
    return m_data.template getUnchecked<1>();
  }

  E &&unwrapErrUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(isErr(), "internal: Result must be Err");
    return std::move(m_data.template getUnchecked<1>());
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
    return isErr() ? Option<E>(std::move(m_data.template getUnchecked<1>())) : Option<E>(none);
  }

  explicit operator bool() const noexcept {
    return isOk();
  }

  /** Like Rust's Result::expect_err. */
  E &expectErr(const char *msg) & {
    XPP_ASSERT(isErr(), "expectErr: %s", msg);
    return m_data.template getUnchecked<1>();
  }
  const E &expectErr(const char *msg) const & {
    XPP_ASSERT(isErr(), "expectErr: %s", msg);
    return m_data.template getUnchecked<1>();
  }
  E &&expectErr(const char *msg) && {
    XPP_ASSERT(isErr(), "expectErr: %s", msg);
    return std::move(m_data.template getUnchecked<1>());
  }

  /** Map E -> F. */
  template <class Func>
  auto mapErr(Func &&fn) const & -> Result<void, decltype(fn(std::declval<const E &>()))> {
    using F = decltype(fn(std::declval<const E &>()));
    return isOk() ? Result<void, F>(xpp::ok) : Result<void, F>(xpp::err, fn(unwrapErrUnchecked()));
  }
  template <class Func>
  auto mapErr(Func &&fn) && -> Result<void, decltype(fn(std::declval<E &&>()))> {
    using F = decltype(fn(std::declval<E &&>()));
    return isOk() ? Result<void, F>(xpp::ok)
                  : Result<void, F>(xpp::err, fn(std::move(unwrapErrUnchecked())));
  }

  /** Monadic bind: fn takes no args and returns Result<U, E>. */
  template <class Func> auto andThen(Func &&fn) const & -> decltype(fn()) {
    using R = decltype(fn());
    return isOk() ? fn() : R(xpp::err, unwrapErrUnchecked());
  }
  template <class Func> auto andThen(Func &&fn) && -> decltype(fn()) {
    using R = decltype(fn());
    return isOk() ? fn() : R(xpp::err, std::move(unwrapErrUnchecked()));
  }

  /** If Err, recover via fn(err) -> Result<void, F>. */
  template <class Func>
  auto orElse(Func &&fn) const & -> decltype(fn(std::declval<const E &>())) {
    using R = decltype(fn(std::declval<const E &>()));
    return isOk() ? R(xpp::ok) : fn(unwrapErrUnchecked());
  }
  template <class Func> auto orElse(Func &&fn) && -> decltype(fn(std::declval<E &&>())) {
    using R = decltype(fn(std::declval<E &&>()));
    return isOk() ? R(xpp::ok) : fn(std::move(unwrapErrUnchecked()));
  }

  /** Chainable side-effect on Err. */
  template <class Func> Result &inspectErr(Func &&fn) & {
    if (isErr()) fn(unwrapErrUnchecked());
    return *this;
  }
  template <class Func> const Result &inspectErr(Func &&fn) const & {
    if (isErr()) fn(unwrapErrUnchecked());
    return *this;
  }
  template <class Func> Result inspectErr(Func &&fn) && {
    if (isErr()) fn(unwrapErrUnchecked());
    return std::move(*this);
  }

private:
  Variant<OkSentinel, E> m_data;
};

/* ── Option::okOr / okOrElse: out-of-line because they depend on Result. ── */

template <class T> template <class E> Result<T, E> Option<T>::okOr(E e) && {
  return m_hasValue ? Result<T, E>(xpp::ok, std::move(unwrapUnchecked()))
                    : Result<T, E>(xpp::err, std::move(e));
}

template <class T>
template <class Func>
auto Option<T>::okOrElse(Func &&fn) && -> Result<T, decltype(fn())> {
  using E = decltype(fn());
  return m_hasValue ? Result<T, E>(xpp::ok, std::move(unwrapUnchecked()))
                    : Result<T, E>(xpp::err, fn());
}

} // namespace xpp

#endif // XPP_RESULT_H
