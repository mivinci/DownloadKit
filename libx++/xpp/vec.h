/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * vec.h - Vec<T>: a growable contiguous buffer.
 *
 * ─── Why "Vec" ──────────────────────────────────────────────────────
 *
 * We call this type Vec (short for Vector) rather than Array because:
 *
 *   1. "Array" in C/C++ universally implies fixed-size (C arrays,
 *      std::array<T,N>). Calling a growable container "Array" invites
 *      confusion about whether it can resize.
 *
 *   2. "Vector" is the established term for a growable contiguous
 *      container in C++ (std::vector) and Rust (Vec). Despite the
 *      name collision with mathematical vectors, 30+ years of
 *      convention have cemented the meaning.
 *
 *   3. We abbreviate to "Vec" (Rust convention) rather than the full
 *      "Vector" for brevity in type signatures — xpp::Vec<xpp::f32>
 *      reads better than xpp::Vector<xpp::f32>. The short form also
 *      leaves room for the full name if a future math-vector type
 *      needs it.
 *
 *   4. For arithmetic T, Vec<T> may in the future gain SIMD-optimized
 *      operations (dot, elementwise add, …) via template specialization,
 *      making the "vector" name accurate in the mathematical sense too.
 *
 * ─── Design ─────────────────────────────────────────────────────────
 *
 * Internally: heap-allocated buffer with pointer + length + capacity.
 * Growth strategy: 2× when capacity is exceeded.
 * Move-only with explicit clone(). No SSO in v1.
 *
 * sizeof(Vec<T>) == sizeof(T*) + 2 * sizeof(size_t) == 24 bytes (64-bit)
 *
 * C++11-compatible. Header-only (all methods are templates).
 */

#ifndef XPP_VEC_H
#define XPP_VEC_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/span.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <utility>

namespace xpp {

/**
 * @brief Growable contiguous buffer. Owns its elements.
 *
 * Move-only. Use clone() for explicit deep copies.
 * Elements are destroyed in reverse order on drop.
 *
 * @tparam T  Element type. Must be move-constructible or copyable.
 */
template <class T> class Vec {
public:
  using value_type = T;
  using size_type  = size_t;
  using pointer    = T *;
  using reference  = T &;

  /* ── Construction ──────────────────────────────────────────────── */

  /** @brief Construct an empty vec. No heap allocation.
   *
   * @code
   *   Vec<int> v;
   *   assert(v.len() == 0 && v.capacity() == 0);
   * @endcode
   */
  Vec() noexcept : m_data(nullptr), m_len(0), m_cap(0) {}

  /**
   * @brief Construct an empty vec with pre-allocated capacity.
   *
   * Useful when the final size is known upfront, avoiding reallocations
   * during subsequent push() calls.
   *
   * @param cap  Number of elements to allocate space for.
   * @return     A new Vec with len()==0 and capacity()>=cap.
   *
   * @code
   *   auto v = Vec<int>::with_capacity(100);
   *   for (int i = 0; i < 100; ++i) v.push(i);  // no realloc
   * @endcode
   */
  static Vec with_capacity(size_t cap) {
    Vec v;
    v.ensure_capacity(cap);
    return v;
  }

  /**
   * @brief Construct a vec with @p count copies of @p value.
   *
   * @param count  Number of elements to create.
   * @param value  Value to copy into each slot (default: T()).
   *
   * @code
   *   Vec<int> zeros(10);       // 10 zeros
   *   Vec<int> sevens(5, 7);   // [7, 7, 7, 7, 7]
   * @endcode
   */
  explicit Vec(size_t count, const T &value = T()) : m_data(nullptr), m_len(0), m_cap(0) {
    reserve(count);
    for (size_t i = 0; i < count; ++i) {
      new (m_data + i) T(value);
    }
    m_len = count;
  }

  ~Vec() {
    destroy_all();
    std::free(m_data);
  }

  /* ── Move-only ─────────────────────────────────────────────────── */

  Vec(Vec &&o) noexcept : m_data(o.m_data), m_len(o.m_len), m_cap(o.m_cap) {
    o.m_data = nullptr;
    o.m_len  = 0;
    o.m_cap  = 0;
  }

  Vec &operator=(Vec &&o) noexcept {
    if (this != &o) {
      destroy_all();
      std::free(m_data);
      m_data   = o.m_data;
      m_len    = o.m_len;
      m_cap    = o.m_cap;
      o.m_data = nullptr;
      o.m_len  = 0;
      o.m_cap  = 0;
    }
    return *this;
  }

  Vec(const Vec &)            = delete;
  Vec &operator=(const Vec &) = delete;

  /* ── Explicit clone ────────────────────────────────────────────── */

  /**
   * @brief Create an independent deep-copy of this vec.
   *
   * Requires T to be copy-constructible. The returned Vec has its own
   * allocation; modifying one does not affect the other.
   *
   * @return A new Vec with identical elements.
   *
   * @code
   *   Vec<int> a; a.push(1); a.push(2);
   *   Vec<int> b = a.clone();
   *   b[0] = 99;
   *   assert(a[0] == 1);  // a unchanged
   * @endcode
   */
  Vec clone() const {
    Vec r;
    r.reserve(m_len);
    for (size_t i = 0; i < m_len; ++i) {
      new (r.m_data + i) T(m_data[i]);
    }
    r.m_len = m_len;
    return r;
  }

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief Raw pointer to the underlying buffer (nullptr if empty). */
  T *data() noexcept { return m_data; }
  /** @brief Raw const pointer to the underlying buffer. */
  const T *data() const noexcept { return m_data; }

  /** @brief Number of elements currently stored. */
  size_t len() const noexcept { return m_len; }
  /** @brief Total number of elements the buffer can hold without reallocating. */
  size_t capacity() const noexcept { return m_cap; }
  /** @brief True if len() == 0. */
  bool is_empty() const noexcept { return m_len == 0; }
  /** @brief STL-compatible alias for is_empty(). */
  bool empty() const noexcept { return m_len == 0; }

  /* ── Element access ────────────────────────────────────────────── */

  /**
   * @brief Access element at @p idx. Debug-bounds-checked.
   *
   * @param idx  Zero-based index. Must be < len().
   * @return     Mutable reference to the element.
   */
  T &operator[](size_t idx) {
    XPP_DEBUG_ASSERT(idx < m_len, "Vec::operator[]: index %zu out of range (len %zu)", idx, m_len);
    return m_data[idx];
  }

  /** @brief Const overload of operator[]. */
  const T &operator[](size_t idx) const {
    XPP_DEBUG_ASSERT(idx < m_len, "Vec::operator[]: index %zu out of range (len %zu)", idx, m_len);
    return m_data[idx];
  }

  /** @brief Reference to the first element. Debug-asserts non-empty. */
  T &front() {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::front() on empty vec");
    return m_data[0];
  }

  const T &front() const {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::front() on empty vec");
    return m_data[0];
  }

  /** @brief Reference to the last element. Debug-asserts non-empty. */
  T &back() {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::back() on empty vec");
    return m_data[m_len - 1];
  }

  const T &back() const {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::back() on empty vec");
    return m_data[m_len - 1];
  }

  /* ── Borrowed views ────────────────────────────────────────────── */

  /** @brief Return a mutable Span over all elements. */
  Span<T> as_span() noexcept { return Span<T>(m_data, m_len); }
  /** @brief Return a const Span over all elements. */
  Span<const T> as_span() const noexcept { return Span<const T>(m_data, m_len); }

  /* ── Mutation ──────────────────────────────────────────────────── */

  /**
   * @brief Append an element to the back (copy).
   *
   * Amortized O(1). May reallocate if capacity is exhausted.
   *
   * @param value  Element to copy-construct at the end.
   *
   * @code
   *   Vec<int> v;
   *   v.push(1);
   *   v.push(2);  // v == [1, 2]
   * @endcode
   */
  void push(const T &value) {
    ensure_capacity(m_len + 1);
    new (m_data + m_len) T(value);
    ++m_len;
  }

  /**
   * @brief Append an element to the back (move).
   *
   * @param value  Element to move-construct at the end.
   */
  void push(T &&value) {
    ensure_capacity(m_len + 1);
    new (m_data + m_len) T(std::move(value));
    ++m_len;
  }

  /**
   * @brief Remove and return the last element.
   *
   * @return Some(element) if non-empty, None if empty.
   *
   * @code
   *   Vec<int> v; v.push(42);
   *   auto val = v.pop();   // val == Some(42), v is empty
   *   auto no  = v.pop();   // no == None
   * @endcode
   */
  Option<T> pop() {
    if (m_len == 0) return none;
    --m_len;
    T val = std::move(m_data[m_len]);
    m_data[m_len].~T();
    return Option<T>(std::move(val));
  }

  /**
   * @brief Copy-append all elements from a span.
   *
   * @param src  Span of elements to copy into this vec.
   *
   * @code
   *   int arr[] = {3, 4, 5};
   *   Vec<int> v; v.push(1); v.push(2);
   *   v.extend(Span<const int>(arr, 3));  // v == [1, 2, 3, 4, 5]
   * @endcode
   */
  void extend(Span<const T> src) {
    if (src.is_empty()) return;
    ensure_capacity(m_len + src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      new (m_data + m_len + i) T(src[i]);
    }
    m_len += src.size();
  }

  /**
   * @brief Remove all elements. Does not free the allocation.
   *
   * After clear(), len()==0 but capacity() is unchanged.
   */
  void clear() {
    destroy_all();
    m_len = 0;
  }

  /**
   * @brief Shorten the vec to @p new_len, dropping trailing elements.
   *
   * Elements beyond new_len are destroyed. Does nothing if new_len == len().
   *
   * @param new_len  Target length. Must be <= len() (debug-asserted).
   *
   * @code
   *   Vec<int> v; v.push(1); v.push(2); v.push(3);
   *   v.truncate(1);  // v == [1]
   * @endcode
   */
  void truncate(size_t new_len) {
    XPP_DEBUG_ASSERT(new_len <= m_len, "Vec::truncate(%zu): exceeds len %zu", new_len, m_len);
    for (size_t i = new_len; i < m_len; ++i) {
      m_data[i].~T();
    }
    m_len = new_len;
  }

  /**
   * @brief Ensure space for at least @p additional more elements.
   *
   * After reserve(n), pushing n elements is guaranteed not to reallocate.
   *
   * @param additional  Extra slots needed beyond current len().
   */
  void reserve(size_t additional) {
    ensure_capacity(m_len + additional);
  }

  /**
   * @brief Shrink the allocation to fit exactly len() elements.
   *
   * Frees excess capacity. If empty, frees the buffer entirely.
   */
  void shrink_to_fit() {
    if (m_cap == m_len) return;
    if (m_len == 0) {
      std::free(m_data);
      m_data = nullptr;
      m_cap  = 0;
      return;
    }
    T *new_buf = static_cast<T *>(std::malloc(m_len * sizeof(T)));
    if (!new_buf) return; /* keep old allocation on failure */
    relocate(new_buf, m_data, m_len);
    std::free(m_data);
    m_data = new_buf;
    m_cap  = m_len;
  }

  /**
   * @brief Insert an element at @p index, shifting subsequent elements right.
   *
   * O(n) due to the shift. For frequent mid-sequence inserts, consider
   * a different data structure.
   *
   * @param index  Position to insert at. Must be <= len() (debug-asserted).
   * @param value  Element to copy-construct at @p index.
   *
   * @code
   *   Vec<int> v; v.push(1); v.push(3);
   *   v.insert(1, 2);  // v == [1, 2, 3]
   * @endcode
   */
  void insert(size_t index, const T &value) {
    XPP_DEBUG_ASSERT(index <= m_len, "Vec::insert(%zu): index exceeds len %zu", index, m_len);
    ensure_capacity(m_len + 1);
    if (index < m_len) {
      shift_right(index, 1);
    }
    new (m_data + index) T(value);
    ++m_len;
  }

  /** @brief Insert by move. Same semantics as the copy overload. */
  void insert(size_t index, T &&value) {
    XPP_DEBUG_ASSERT(index <= m_len, "Vec::insert(%zu): index exceeds len %zu", index, m_len);
    ensure_capacity(m_len + 1);
    if (index < m_len) {
      shift_right(index, 1);
    }
    new (m_data + index) T(std::move(value));
    ++m_len;
  }

  /**
   * @brief Remove and return the element at @p index, shifting subsequent
   *        elements left to fill the gap.
   *
   * O(n) due to the shift. Preserves element order.
   *
   * @param index  Position to remove. Must be < len() (debug-asserted).
   * @return       The removed element (moved out).
   *
   * @code
   *   Vec<int> v; v.push(1); v.push(2); v.push(3);
   *   int x = v.remove(1);  // x == 2, v == [1, 3]
   * @endcode
   */
  T remove(size_t index) {
    XPP_DEBUG_ASSERT(index < m_len, "Vec::remove(%zu): index out of range (len %zu)", index, m_len);
    T val = std::move(m_data[index]);
    m_data[index].~T();
    if (index + 1 < m_len) {
      shift_left(index, 1);
    }
    --m_len;
    return val;
  }

  /**
   * @brief Remove the element at @p index by swapping with the last. O(1).
   *
   * Does NOT preserve element order. Use when order doesn't matter and
   * O(1) removal is needed.
   *
   * @param index  Position to remove. Must be < len() (debug-asserted).
   * @return       The removed element (moved out).
   *
   * @code
   *   Vec<int> v; v.push(10); v.push(20); v.push(30); v.push(40);
   *   int x = v.swap_remove(1);  // x == 20, v == [10, 40, 30]
   * @endcode
   */
  T swap_remove(size_t index) {
    XPP_DEBUG_ASSERT(index < m_len,
                     "Vec::swap_remove(%zu): index out of range (len %zu)", index, m_len);
    T val = std::move(m_data[index]);
    m_data[index].~T();
    --m_len;
    if (index < m_len) {
      /* Move last element into the vacated slot. */
      new (m_data + index) T(std::move(m_data[m_len]));
      m_data[m_len].~T();
    }
    return val;
  }

  /**
   * @brief Move all elements from @p other into this vec, draining @p other.
   *
   * After this call, other.is_empty() == true. Elements are appended in order.
   *
   * @param other  Vec to drain. Left empty after the call.
   *
   * @code
   *   Vec<int> a; a.push(1); a.push(2);
   *   Vec<int> b; b.push(3); b.push(4);
   *   a.append(b);  // a == [1, 2, 3, 4], b == []
   * @endcode
   */
  void append(Vec &other) {
    if (other.is_empty()) return;
    ensure_capacity(m_len + other.m_len);
    for (size_t i = 0; i < other.m_len; ++i) {
      new (m_data + m_len + i) T(std::move(other.m_data[i]));
      other.m_data[i].~T();
    }
    m_len += other.m_len;
    other.m_len = 0;
  }

  /**
   * @brief Resize the vec to @p new_len.
   *
   * If new_len > len(), appends copies of @p value to fill.
   * If new_len < len(), truncates (drops trailing elements).
   * If new_len == len(), does nothing.
   *
   * @param new_len  Desired length.
   * @param value    Fill value for new slots (default: T()).
   *
   * @code
   *   Vec<int> v; v.push(1);
   *   v.resize(4, 0);   // v == [1, 0, 0, 0]
   *   v.resize(2);      // v == [1, 0]
   * @endcode
   */
  void resize(size_t new_len, const T &value = T()) {
    if (new_len <= m_len) {
      truncate(new_len);
      return;
    }
    ensure_capacity(new_len);
    for (size_t i = m_len; i < new_len; ++i) {
      new (m_data + i) T(value);
    }
    m_len = new_len;
  }

  /**
   * @brief Keep only elements for which @p pred returns true.
   *
   * Elements are visited in order. Removed elements are destroyed.
   * Relative order of kept elements is preserved.
   *
   * @param pred  Callable with signature `bool(const T&)` or `bool(T&)`.
   *
   * @code
   *   Vec<int> v; for (int i=1; i<=6; ++i) v.push(i);
   *   v.retain([](int x) { return x % 2 == 0; });  // v == [2, 4, 6]
   * @endcode
   */
  template <class Pred> void retain(Pred pred) {
    size_t write = 0;
    for (size_t read = 0; read < m_len; ++read) {
      if (pred(m_data[read])) {
        if (write != read) {
          new (m_data + write) T(std::move(m_data[read]));
          m_data[read].~T();
        }
        ++write;
      } else {
        m_data[read].~T();
      }
    }
    m_len = write;
  }

  /* ── Iterators (raw pointers) ──────────────────────────────────── */

  /** @brief Pointer to the first element (for range-for and STL algorithms). */
  T *begin() noexcept { return m_data; }
  /** @brief Pointer past the last element. */
  T *end() noexcept { return m_data + m_len; }
  /** @brief Const pointer to the first element. */
  const T *begin() const noexcept { return m_data; }
  /** @brief Const pointer past the last element. */
  const T *end() const noexcept { return m_data + m_len; }

  /* ── Comparison (element-wise) ─────────────────────────────────── */

  /**
   * @brief Element-wise equality. Two Vecs are equal iff they have
   *        the same length and all corresponding elements compare equal.
   */
  friend bool operator==(const Vec &a, const Vec &b) {
    if (a.m_len != b.m_len) return false;
    for (size_t i = 0; i < a.m_len; ++i) {
      if (!(a.m_data[i] == b.m_data[i])) return false;
    }
    return true;
  }

  friend bool operator!=(const Vec &a, const Vec &b) { return !(a == b); }

private:
  T     *m_data;
  size_t m_len;
  size_t m_cap;

  void ensure_capacity(size_t required) {
    if (required <= m_cap) return;
    size_t new_cap = m_cap == 0 ? 4 : m_cap;
    while (new_cap < required) new_cap *= 2;
    T *new_buf = static_cast<T *>(std::malloc(new_cap * sizeof(T)));
    XPP_ASSERT(new_buf != nullptr, "Vec: allocation failed");
    if (m_len > 0) {
      relocate(new_buf, m_data, m_len);
    }
    std::free(m_data);
    m_data = new_buf;
    m_cap  = new_cap;
  }

  /** @brief Move-construct elements from src to dst, destroy src. */
  static void relocate(T *dst, T *src, size_t count) {
    /* For trivially-copyable types, memcpy is optimal. */
    relocate_impl(dst, src, count, std::is_trivially_copyable<T>{});
  }

  static void relocate_impl(T *dst, T *src, size_t count, std::true_type /*trivial*/) {
    std::memcpy(dst, src, count * sizeof(T));
  }

  static void relocate_impl(T *dst, T *src, size_t count, std::false_type /*non-trivial*/) {
    for (size_t i = 0; i < count; ++i) {
      new (dst + i) T(std::move(src[i]));
      src[i].~T();
    }
  }

  void destroy_all() {
    destroy_all_impl(std::is_trivially_destructible<T>{});
  }

  void destroy_all_impl(std::true_type /*trivial_dtor*/) {
    /* No-op for trivially destructible types. */
  }

  void destroy_all_impl(std::false_type /*non-trivial_dtor*/) {
    for (size_t i = m_len; i > 0; --i) {
      m_data[i - 1].~T();
    }
  }

  /** @brief Shift elements [index..len) right by @p count slots. */
  void shift_right(size_t index, size_t count) {
    shift_impl(index, count, std::is_trivially_copyable<T>{});
  }

  void shift_impl(size_t index, size_t count, std::true_type /*trivial*/) {
    std::memmove(m_data + index + count, m_data + index, (m_len - index) * sizeof(T));
  }

  void shift_impl(size_t index, size_t count, std::false_type /*non-trivial*/) {
    /* Move-construct from back to front to avoid overwrites. */
    for (size_t i = m_len; i > index; --i) {
      new (m_data + i - 1 + count) T(std::move(m_data[i - 1]));
      m_data[i - 1].~T();
    }
  }

  /** @brief Shift elements [index+count..len) left by @p count slots. */
  void shift_left(size_t index, size_t count) {
    shift_left_impl(index, count, std::is_trivially_copyable<T>{});
  }

  void shift_left_impl(size_t index, size_t count, std::true_type /*trivial*/) {
    std::memmove(m_data + index, m_data + index + count, (m_len - index - count) * sizeof(T));
  }

  void shift_left_impl(size_t index, size_t count, std::false_type /*non-trivial*/) {
    size_t remaining = m_len - index - count;
    for (size_t i = 0; i < remaining; ++i) {
      new (m_data + index + i) T(std::move(m_data[index + count + i]));
      m_data[index + count + i].~T();
    }
  }
};

/* ── Compile-time size guarantees ────────────────────────────────── */

static_assert(sizeof(Vec<int>) == sizeof(int *) + 2 * sizeof(size_t),
              "Vec<T> must be pointer + len + cap, no overhead");

} // namespace xpp

#endif // XPP_VEC_H
