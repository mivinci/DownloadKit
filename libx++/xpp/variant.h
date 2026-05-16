/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * variant.h - Type-safe tagged union for exactly one of N types.
 *
 * C++14-compatible replacement for std::variant.
 */

#ifndef XPP_VARIANT_H
#define XPP_VARIANT_H

#include "in_place.h"
#include "panic.h"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace xpp {
namespace _ {

template <size_t I, typename T, typename... Types> struct TypeIndex;
template <size_t I, typename T, typename First, typename... Rest>
struct TypeIndex<I, T, First, Rest...> {
  static constexpr size_t kValue =
    std::is_same<T, First>::value ? I : TypeIndex<I + 1, T, Rest...>::kValue;
};
template <size_t I, typename T> struct TypeIndex<I, T> {
  static constexpr size_t kValue = I;
};

// Call fn(holder) where holder is a Holder<T> for the active type.
// fn should return void.
template <typename Tuple, size_t N> struct VisitByIndex {
  template <typename Fn, typename Storage> static void run(size_t i, Storage &storage, Fn &&fn) {
    if (i == N - 1) {
      using T = typename std::tuple_element<N - 1, Tuple>::type;
      fn(reinterpret_cast<T *>(&storage));
      return;
    }
    VisitByIndex<Tuple, N - 1>::run(i, storage, std::forward<Fn>(fn));
  }
};
template <typename Tuple> struct VisitByIndex<Tuple, 0> {
  template <typename Fn, typename Storage> static void run(size_t, Storage &, Fn &&) {}
};

} // namespace _

/**
 * @brief Type-safe tagged union holding exactly one of Types...
 *
 * Always holds a value (no empty/default state). C++14-compatible.
 *
 * Usage:
 *   Variant<int, float> a(42);       // holds int
 *   Variant<int, float> b(3.14f);    // holds float
 *   a.is<int>();                    // true
 *   a.get<int>();                   // 42
 */
template <typename... Types> class Variant {
  static constexpr size_t kCount = sizeof...(Types);
  static_assert(kCount >= 2, "Variant requires at least two types");
  using Tuple = std::tuple<Types...>;

public:
  /** Construct from a value of one of the Types. */
  template <typename T, typename = typename std::enable_if<
                          !std::is_same<typename std::decay<T>::type, Variant>::value>::type>
  Variant(T &&val) : m_index(indexOf<typename std::decay<T>::type>()) {
    using D = typename std::decay<T>::type;
    new (&m_storage) D(std::forward<T>(val));
  }

  /**
   * @brief Construct the N-th alternative in place from @p args.
   *
   * Disambiguates when multiple Types share the same type (e.g.
   * Variant<int, int>) or when explicit selection is desired.
   *
   * @tparam N     Index into Types... (must be < sizeof...(Types)).
   * @tparam Args  Constructor argument types for the selected type.
   * @param  args  Forwarded to the selected type's constructor.
   *
   * Usage:
   *   Variant<int, std::string> a(InPlaceIndex<1>{}, "hi");
   */
  template <size_t N, typename... Args> Variant(InPlaceIndex<N>, Args &&...args) : m_index(N) {
    static_assert(N < kCount, "InPlaceIndex out of range");
    using T = typename std::tuple_element<N, Tuple>::type;
    new (&m_storage) T(std::forward<Args>(args)...);
  }

  Variant(const Variant &o) : m_index(o.m_index) {
    copyFrom(o);
  }

  Variant(Variant &&o) noexcept : m_index(o.m_index) {
    moveFrom(std::move(o));
  }

  ~Variant() {
    destroy();
  }

  Variant &operator=(const Variant &o) {
    if (this != &o) {
      destroy();
      m_index = o.m_index;
      copyFrom(o);
    }
    return *this;
  }

  Variant &operator=(Variant &&o) noexcept {
    if (this != &o) {
      destroy();
      m_index = o.m_index;
      moveFrom(std::move(o));
    }
    return *this;
  }

  /** True if this currently holds type T. */
  template <typename T> bool is() const noexcept {
    return m_index == indexOf<T>();
  }

  /** True if this currently holds the N-th alternative. */
  template <size_t N> bool is() const noexcept {
    static_assert(N < kCount, "index out of range");
    return m_index == N;
  }

  /**
   * @brief Get reference to the held T.
   *
   * Panics if !is<T>(). For zero-cost access when the caller has already
   * verified the active alternative, use getUnchecked<T>().
   */
  template <typename T> T &get() & {
    XPP_ASSERT(is<T>(), "get<T>() on Variant holding a different type");
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <typename T> const T &get() const & {
    XPP_ASSERT(is<T>(), "get<T>() on Variant holding a different type");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <typename T> T &&get() && {
    XPP_ASSERT(is<T>(), "get<T>() on Variant holding a different type");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get reference to the held T without checking. UB if !is<T>().
   *
   * Debug builds assert; release builds elide the check. Caller must
   * ensure is<T>().
   */
  template <typename T> T &getUnchecked() & noexcept {
    XPP_DEBUG_ASSERT(is<T>(), "internal: Variant must hold T");
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <typename T> const T &getUnchecked() const & noexcept {
    XPP_DEBUG_ASSERT(is<T>(), "internal: Variant must hold T");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <typename T> T &&getUnchecked() && noexcept {
    XPP_DEBUG_ASSERT(is<T>(), "internal: Variant must hold T");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get reference to the N-th alternative.
   *
   * Panics if !is<N>(). Use this overload to disambiguate when Types
   * contains duplicates (e.g. Variant<int, int>), where get<T>() is
   * unambiguous only for unique T.
   */
  template <size_t N> typename std::tuple_element<N, Tuple>::type &get() & {
    static_assert(N < kCount, "index out of range");
    XPP_ASSERT(m_index == N, "get<N>() on Variant holding a different alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <size_t N> const typename std::tuple_element<N, Tuple>::type &get() const & {
    static_assert(N < kCount, "index out of range");
    XPP_ASSERT(m_index == N, "get<N>() on Variant holding a different alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <size_t N> typename std::tuple_element<N, Tuple>::type &&get() && {
    static_assert(N < kCount, "index out of range");
    XPP_ASSERT(m_index == N, "get<N>() on Variant holding a different alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get reference to the N-th alternative without checking.
   *
   * Debug builds assert; release builds elide the check. Caller must
   * ensure is<N>().
   */
  template <size_t N> typename std::tuple_element<N, Tuple>::type &getUnchecked() & noexcept {
    static_assert(N < kCount, "index out of range");
    XPP_DEBUG_ASSERT(m_index == N, "internal: Variant must hold N-th alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <size_t N>
  const typename std::tuple_element<N, Tuple>::type &getUnchecked() const & noexcept {
    static_assert(N < kCount, "index out of range");
    XPP_DEBUG_ASSERT(m_index == N, "internal: Variant must hold N-th alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <size_t N> typename std::tuple_element<N, Tuple>::type &&getUnchecked() && noexcept {
    static_assert(N < kCount, "index out of range");
    XPP_DEBUG_ASSERT(m_index == N, "internal: Variant must hold N-th alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /** Zero-based index of the currently held type. */
  size_t index() const noexcept {
    return m_index;
  }

private:
  template <typename T> static constexpr size_t indexOf() {
    return _::TypeIndex<0, T, Types...>::kValue;
  }

  void destroy() {
    _::VisitByIndex<Tuple, kCount>::run(m_index, m_storage, [](auto *ptr) {
      using T = typename std::remove_pointer<decltype(ptr)>::type;
      ptr->~T();
    });
    m_index = kCount;
  }

  void copyFrom(const Variant &o) {
    _::VisitByIndex<Tuple, kCount>::run(o.m_index, o.m_storage, [this](auto *ptr) {
      using T = typename std::remove_pointer<decltype(ptr)>::type;
      new (&m_storage) T(*ptr);
    });
  }

  void moveFrom(Variant &&o) {
    _::VisitByIndex<Tuple, kCount>::run(o.m_index, o.m_storage, [this](auto *ptr) {
      using T = typename std::remove_pointer<decltype(ptr)>::type;
      new (&m_storage) T(std::move(*ptr));
    });
  }

  using Storage = typename std::aligned_union<0, Types...>::type;
  Storage m_storage;
  size_t  m_index;
};

} // namespace xpp

#endif // XPP_VARIANT_H
