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
#include <new>
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

  /** @brief Empty vec. No allocation. */
  Vec() noexcept : m_data(nullptr), m_len(0), m_cap(0) {}

  /** @brief Construct with pre-allocated capacity. No elements. */
  static Vec with_capacity(size_t cap) {
    Vec v;
    v.ensure_capacity(cap);
    return v;
  }

  /** @brief Construct with @p count copies of @p value. */
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

  /** @brief Deep-copy. Requires T to be copy-constructible. */
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

  T *data() noexcept { return m_data; }
  const T *data() const noexcept { return m_data; }

  size_t len() const noexcept { return m_len; }
  size_t capacity() const noexcept { return m_cap; }
  bool is_empty() const noexcept { return m_len == 0; }
  bool empty() const noexcept { return m_len == 0; }

  /* ── Element access ────────────────────────────────────────────── */

  T &operator[](size_t idx) {
    XPP_DEBUG_ASSERT(idx < m_len, "Vec::operator[]: index %zu out of range (len %zu)", idx, m_len);
    return m_data[idx];
  }

  const T &operator[](size_t idx) const {
    XPP_DEBUG_ASSERT(idx < m_len, "Vec::operator[]: index %zu out of range (len %zu)", idx, m_len);
    return m_data[idx];
  }

  T &front() {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::front() on empty vec");
    return m_data[0];
  }

  const T &front() const {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::front() on empty vec");
    return m_data[0];
  }

  T &back() {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::back() on empty vec");
    return m_data[m_len - 1];
  }

  const T &back() const {
    XPP_DEBUG_ASSERT(m_len > 0, "Vec::back() on empty vec");
    return m_data[m_len - 1];
  }

  /* ── Borrowed views ────────────────────────────────────────────── */

  Span<T> as_span() noexcept { return Span<T>(m_data, m_len); }
  Span<const T> as_span() const noexcept { return Span<const T>(m_data, m_len); }

  /* ── Mutation ──────────────────────────────────────────────────── */

  /** @brief Append an element by copy. */
  void push(const T &value) {
    ensure_capacity(m_len + 1);
    new (m_data + m_len) T(value);
    ++m_len;
  }

  /** @brief Append an element by move. */
  void push(T &&value) {
    ensure_capacity(m_len + 1);
    new (m_data + m_len) T(std::move(value));
    ++m_len;
  }

  /** @brief Remove and return the last element. */
  Option<T> pop() {
    if (m_len == 0) return none;
    --m_len;
    T val = std::move(m_data[m_len]);
    m_data[m_len].~T();
    return Option<T>(std::move(val));
  }

  /** @brief Append all elements from a span. */
  void extend(Span<const T> src) {
    if (src.is_empty()) return;
    ensure_capacity(m_len + src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      new (m_data + m_len + i) T(src[i]);
    }
    m_len += src.size();
  }

  /** @brief Remove all elements (does not free allocation). */
  void clear() {
    destroy_all();
    m_len = 0;
  }

  /** @brief Truncate to @p new_len elements. */
  void truncate(size_t new_len) {
    XPP_DEBUG_ASSERT(new_len <= m_len, "Vec::truncate(%zu): exceeds len %zu", new_len, m_len);
    for (size_t i = new_len; i < m_len; ++i) {
      m_data[i].~T();
    }
    m_len = new_len;
  }

  /** @brief Ensure capacity for at least @p additional more elements. */
  void reserve(size_t additional) {
    ensure_capacity(m_len + additional);
  }

  /** @brief Shrink allocation to fit current length. */
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

  /** @brief Insert an element at @p index, shifting subsequent elements right. */
  void insert(size_t index, const T &value) {
    XPP_DEBUG_ASSERT(index <= m_len, "Vec::insert(%zu): index exceeds len %zu", index, m_len);
    ensure_capacity(m_len + 1);
    if (index < m_len) {
      shift_right(index, 1);
    }
    new (m_data + index) T(value);
    ++m_len;
  }

  /** @brief Insert an element by move at @p index. */
  void insert(size_t index, T &&value) {
    XPP_DEBUG_ASSERT(index <= m_len, "Vec::insert(%zu): index exceeds len %zu", index, m_len);
    ensure_capacity(m_len + 1);
    if (index < m_len) {
      shift_right(index, 1);
    }
    new (m_data + index) T(std::move(value));
    ++m_len;
  }

  /** @brief Remove and return element at @p index, shifting subsequent elements left. */
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
   * @brief Remove element at @p index by swapping with the last element. O(1).
   *
   * Does not preserve order. Returns the removed element.
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
   * @brief Move all elements from @p other into this vec.
   *
   * @p other is left empty after this call.
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
   * @brief Resize to @p new_len. If growing, fill new slots with @p value.
   *
   * If new_len < len(), truncates. If new_len > len(), appends copies.
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

  T *begin() noexcept { return m_data; }
  T *end() noexcept { return m_data + m_len; }
  const T *begin() const noexcept { return m_data; }
  const T *end() const noexcept { return m_data + m_len; }

  /* ── Comparison (element-wise) ─────────────────────────────────── */

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
