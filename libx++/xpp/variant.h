/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * variant.h - Type-safe tagged union for exactly one of N types.
 *
 * C++11-compatible replacement for std::variant.
 */

#ifndef XPP_VARIANT_H
#define XPP_VARIANT_H

#include <xpp/in_place.h>
#include <xpp/panic.h>

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace xpp {
namespace _ {

template <size_t I, class T, class... Types> struct TypeIndex;
template <size_t I, class T, class First, class... Rest> struct TypeIndex<I, T, First, Rest...> {
  static constexpr size_t k_value =
    std::is_same<T, First>::value ? I : TypeIndex<I + 1, T, Rest...>::k_value;
};
template <size_t I, class T> struct TypeIndex<I, T> {
  static constexpr size_t k_value = I;
};

// Visit the active alternative of a Variant by its runtime index.
// fn(ptr) is invoked exactly once with a typed pointer to the live
// object (T* or const T*, mirroring the constness of `storage`); the
// recursion bottoms out at the matching index. fn should return void.
//
// `PointerCast` carries the constness from Storage through to T so
// reinterpret_cast<T*>(&storage) doesn't silently strip const when
// the dispatcher is called from a const context (e.g. copy_from).
template <class T, class Storage>
struct PointerCast {
  using Type = typename std::conditional<std::is_const<Storage>::value, const T *, T *>::type;
};

template <class Tuple, size_t N> struct VisitByIndex {
  template <class Fn, class Storage> static void run(size_t i, Storage &storage, Fn &&fn) {
    if (i == N - 1) {
      using T = typename std::tuple_element<N - 1, Tuple>::type;
      using P = typename PointerCast<T, Storage>::Type;
      fn(reinterpret_cast<P>(&storage));
      return;
    }
    VisitByIndex<Tuple, N - 1>::run(i, storage, std::forward<Fn>(fn));
  }
};
template <class Tuple> struct VisitByIndex<Tuple, 0> {
  template <class Fn, class Storage> static void run(size_t, Storage &, Fn &&) {}
};

// Functor-style visitors used by Variant. Generic lambdas (C++14)
// would express these in three lines each; explicit functors keep
// libx++ buildable on C++11 toolchains.

struct DestroyVisitor {
  template <class T> void operator()(T *p) const noexcept(noexcept(p->~T())) {
    p->~T();
  }
};

template <class Storage> struct CopyConstructVisitor {
  Storage                *dst;
  template <class T> void operator()(const T *src) const {
    new (dst) T(*src);
  }
};

template <class Storage> struct MoveConstructVisitor {
  Storage                *dst;
  template <class T> void operator()(T *src) const {
    new (dst) T(std::move(*src));
  }
};

} // namespace _

/**
 * @brief Type-safe tagged union holding exactly one of Types...
 *
 * Always holds a value (no empty/default state). C++11-compatible.
 *
 * Usage:
 *   Variant<int, float> a(42);       // holds int
 *   Variant<int, float> b(3.14f);    // holds float
 *   a.is<int>();                    // true
 *   a.get<int>();                   // 42
 */
template <class... Types> class Variant {
  static constexpr size_t k_count = sizeof...(Types);
  static_assert(k_count >= 2, "Variant requires at least two types");
  using Tuple = std::tuple<Types...>;

public:
  /** Construct from a value of one of the Types. */
  template <class T, class = typename std::enable_if<
                       !std::is_same<typename std::decay<T>::type, Variant>::value>::type>
  Variant(T &&val) : m_index(index_of<typename std::decay<T>::type>()) {
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
  template <size_t N, class... Args> Variant(InPlaceIndex<N>, Args &&...args) : m_index(N) {
    static_assert(N < k_count, "InPlaceIndex out of range");
    using T = typename std::tuple_element<N, Tuple>::type;
    new (&m_storage) T(std::forward<Args>(args)...);
  }

  Variant(const Variant &o) : m_index(o.m_index) {
    copy_from(o);
  }

  Variant(Variant &&o) noexcept : m_index(o.m_index) {
    move_from(std::move(o));
  }

  ~Variant() {
    destroy();
  }

  Variant &operator=(const Variant &o) {
    if (this != &o) {
      destroy();
      m_index = o.m_index;
      copy_from(o);
    }
    return *this;
  }

  Variant &operator=(Variant &&o) noexcept {
    if (this != &o) {
      destroy();
      m_index = o.m_index;
      move_from(std::move(o));
    }
    return *this;
  }

  /** True if this currently holds type T. */
  template <class T> bool is() const noexcept {
    return m_index == index_of<T>();
  }

  /** True if this currently holds the N-th alternative. */
  template <size_t N> bool is() const noexcept {
    static_assert(N < k_count, "index out of range");
    return m_index == N;
  }

  /**
   * @brief Get reference to the held T.
   *
   * Panics if !is<T>(). For zero-cost access when the caller has already
   * verified the active alternative, use get_unchecked<T>().
   */
  template <class T> T &get() & {
    XPP_ASSERT(is<T>(), "get<T>() on Variant holding a different type");
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <class T> const T &get() const & {
    XPP_ASSERT(is<T>(), "get<T>() on Variant holding a different type");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <class T> T &&get() && {
    XPP_ASSERT(is<T>(), "get<T>() on Variant holding a different type");
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /**
   * @brief Get reference to the held T without checking. UB if !is<T>().
   *
   * Debug builds assert; release builds elide the check. Caller must
   * ensure is<T>().
   */
  template <class T> T &get_unchecked() & noexcept {
    XPP_DEBUG_ASSERT(is<T>(), "internal: Variant must hold T");
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <class T> const T &get_unchecked() const & noexcept {
    XPP_DEBUG_ASSERT(is<T>(), "internal: Variant must hold T");
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <class T> T &&get_unchecked() && noexcept {
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
    static_assert(N < k_count, "index out of range");
    XPP_ASSERT(m_index == N, "get<N>() on Variant holding a different alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <size_t N> const typename std::tuple_element<N, Tuple>::type &get() const & {
    static_assert(N < k_count, "index out of range");
    XPP_ASSERT(m_index == N, "get<N>() on Variant holding a different alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <size_t N> typename std::tuple_element<N, Tuple>::type &&get() && {
    static_assert(N < k_count, "index out of range");
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
  template <size_t N> typename std::tuple_element<N, Tuple>::type &get_unchecked() & noexcept {
    static_assert(N < k_count, "index out of range");
    XPP_DEBUG_ASSERT(m_index == N, "internal: Variant must hold N-th alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<T *>(&m_storage);
  }

  template <size_t N>
  const typename std::tuple_element<N, Tuple>::type &get_unchecked() const & noexcept {
    static_assert(N < k_count, "index out of range");
    XPP_DEBUG_ASSERT(m_index == N, "internal: Variant must hold N-th alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return *reinterpret_cast<const T *>(&m_storage);
  }

  template <size_t N> typename std::tuple_element<N, Tuple>::type &&get_unchecked() && noexcept {
    static_assert(N < k_count, "index out of range");
    XPP_DEBUG_ASSERT(m_index == N, "internal: Variant must hold N-th alternative");
    using T = typename std::tuple_element<N, Tuple>::type;
    return std::move(*reinterpret_cast<T *>(&m_storage));
  }

  /** Zero-based index of the currently held type. */
  size_t index() const noexcept {
    return m_index;
  }

private:
  template <class T> static constexpr size_t index_of() {
    return _::TypeIndex<0, T, Types...>::k_value;
  }

  void destroy() {
    _::VisitByIndex<Tuple, k_count>::run(m_index, m_storage, _::DestroyVisitor{});
    m_index = k_count;
  }

  void copy_from(const Variant &o) {
    _::VisitByIndex<Tuple, k_count>::run(o.m_index, o.m_storage,
                                        _::CopyConstructVisitor<Storage>{&m_storage});
  }

  void move_from(Variant &&o) {
    _::VisitByIndex<Tuple, k_count>::run(o.m_index, o.m_storage,
                                        _::MoveConstructVisitor<Storage>{&m_storage});
  }

  using Storage = typename std::aligned_union<0, Types...>::type;
  Storage m_storage;
  size_t  m_index;
};

} // namespace xpp

#endif // XPP_VARIANT_H
