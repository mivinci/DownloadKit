/*
 * Copyright 2025 The libxpp Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * own.h - Own<T, Deleter>: a nullable owning smart pointer with Rust-style
 *        / std::unique_ptr-style API.
 *
 * Storage is Option<NonNullOwn<T, Deleter>> directly, so:
 *   sizeof(Own<T>) == sizeof(T*)              (default_delete is empty → EBO)
 *   sizeof(Own<T, StatefulD>) == sizeof(T*) + sizeof(StatefulD)
 *
 * Relationship to NonNullOwn<T>:
 *   - Own<T>          — may be null. Default ctor → null. reset()/release()/take().
 *                       operator* and operator-> debug-assert on null.
 *   - NonNullOwn<T>   — type-level non-null. No reset, no default ctor, no null state.
 *   - Bridge:
 *       std::move(own).intoNonNull()           -> Option<NonNullOwn<T, D>>
 *       Own<T>(std::move(opt_nonnullown))      <- adopt back from Option
 *
 * Choose Own<T> when you want C++/Rust-idiomatic ownership (operator*, reset,
 * if(o)). Choose NonNullOwn<T> + Option when you want type-level guarantees
 * and Rust-style combinators.
 *
 * C++14-compatible. Header-only.
 */

#ifndef XPP_OWN_H
#define XPP_OWN_H

#include "nonnull_own.h"
#include "option.h"
#include "panic.h"

#include <memory>
#include <type_traits>
#include <utility>

namespace xpp {

/**
 * @brief Nullable owning smart pointer. Rust/std::unique_ptr-style API.
 *
 * Move-only. Exception-free destructor. Holds at most one heap-allocated T,
 * disposed via Deleter on destruction or reset.
 *
 * @tparam T        Pointee type. T = void supported (operator*, ->
 *                  SFINAE-removed).
 * @tparam Deleter  Function-object-like type called on the held pointer
 *                  when non-null. Defaults to std::default_delete<T>.
 */
template <class T, class Deleter = std::default_delete<T>>
class Own {
  using Inner = Option<NonNullOwn<T, Deleter>>;

public:
  using element_type = T;
  using deleter_type = Deleter;
  using pointer      = T *;

  /** @brief Default ctor. Constructs an empty (null) Own. */
  Own() noexcept = default;

  /** @brief Construct empty from nullptr_t literal. */
  Own(std::nullptr_t) noexcept {}

  /**
   * @brief Take ownership of a raw pointer (may be null).
   *
   * If `p` is null, the resulting Own is empty. Otherwise it owns `p`.
   */
  explicit Own(T *p) noexcept : m_inner(NonNullOwn<T, Deleter>::from(p)) {}

  /** @brief Take ownership of a raw pointer with a custom deleter instance. */
  Own(T *p, Deleter d) noexcept
      : m_inner(NonNullOwn<T, Deleter>::from(p, std::move(d))) {}

  /** @brief Adopt an existing NonNullOwn (always non-empty). */
  Own(NonNullOwn<T, Deleter> &&nn) noexcept : m_inner(std::move(nn)) {}

  /** @brief Adopt from Option<NonNullOwn>. Empty iff the Option is None. */
  Own(Option<NonNullOwn<T, Deleter>> &&opt) noexcept : m_inner(std::move(opt)) {}

  /** @brief Covariant: adopt NonNullOwn<Derived, E>. */
  template <class U, class E,
            class = typename std::enable_if<
                std::is_convertible<U *, T *>::value && !std::is_same<U, T>::value &&
                std::is_convertible<E &&, Deleter>::value>::type>
  Own(NonNullOwn<U, E> &&nn) noexcept : m_inner(std::move(nn)) {}

  /** @brief Covariant: adopt Option<NonNullOwn<Derived, E>>. */
  template <class U, class E,
            class = typename std::enable_if<
                std::is_convertible<U *, T *>::value && !std::is_same<U, T>::value &&
                std::is_convertible<E &&, Deleter>::value>::type>
  Own(Option<NonNullOwn<U, E>> &&opt) noexcept : m_inner(std::move(opt)) {}

  /** @brief Covariant: Own<Derived, E> → Own<Base, D>. */
  template <class U, class E,
            class = typename std::enable_if<
                std::is_convertible<U *, T *>::value && !std::is_same<U, T>::value &&
                std::is_convertible<E &&, Deleter>::value>::type>
  Own(Own<U, E> &&other) noexcept : m_inner(std::move(other.m_inner)) {}

  Own(const Own &)            = delete;
  Own &operator=(const Own &) = delete;

  Own(Own &&) noexcept            = default;
  Own &operator=(Own &&) noexcept = default;

  ~Own() = default;

  /** @brief Reset to empty; deletes any currently held object. */
  Own &operator=(std::nullptr_t) noexcept {
    m_inner = none;
    return *this;
  }

  /** @brief Replace held pointer. Old object (if any) is deleted. */
  void reset(T *p = nullptr) noexcept {
    m_inner = NonNullOwn<T, Deleter>::from(p);
  }

  /**
   * @brief Relinquish ownership; return raw pointer (may be null).
   *
   * Rust-style name. Equivalent to `release()`.
   */
  T *take() noexcept {
    if (m_inner.isNone()) return nullptr;
    return std::move(m_inner).unwrapUnchecked().release();
  }

  /**
   * @brief Relinquish ownership; return raw pointer (may be null).
   *
   * std::unique_ptr-style name. Equivalent to `take()`.
   */
  T *release() noexcept { return take(); }

  /** @brief Get raw pointer; null if empty. */
  T *get() const noexcept {
    return m_inner.isSome() ? m_inner.unwrapUnchecked() : nullptr;
  }

  /** @brief Dereference. Debug-asserts non-empty; UB in release on empty. */
  template <class U = T,
            class   = typename std::enable_if<!std::is_void<U>::value>::type>
  U &operator*() const noexcept {
    XPP_DEBUG_ASSERT(m_inner.isSome(), "Own::operator* on empty Own");
    return *m_inner.unwrapUnchecked();
  }

  template <class U = T,
            class   = typename std::enable_if<!std::is_void<U>::value>::type>
  U *operator->() const noexcept {
    XPP_DEBUG_ASSERT(m_inner.isSome(), "Own::operator-> on empty Own");
    return m_inner.unwrapUnchecked();
  }

  /** @brief True iff non-empty. */
  explicit operator bool() const noexcept { return m_inner.isSome(); }

  bool operator==(std::nullptr_t) const noexcept { return m_inner.isNone(); }
  bool operator!=(std::nullptr_t) const noexcept { return m_inner.isSome(); }

  /**
   * @brief Consume into Option<NonNullOwn>. Bridges to the Rust-style API.
   *
   * If the Own was empty, returns None. Otherwise Some(NonNullOwn). To
   * inspect or take the deleter, do `std::move(own).intoNonNull().unwrap().getDeleter()`.
   */
  Option<NonNullOwn<T, Deleter>> intoNonNull() && noexcept {
    return std::move(m_inner);
  }

private:
  Inner m_inner;

  // Allow covariant ctor to reach into another instantiation's storage.
  template <class, class> friend class Own;
};

/* ── Compile-time size guarantees ────────────────────────────────────── */

static_assert(sizeof(Own<int>) == sizeof(int *),
              "Own<T, default_delete> must be sizeof(T*) via niche-optimized Option storage");

} // namespace xpp

#endif // XPP_OWN_H
