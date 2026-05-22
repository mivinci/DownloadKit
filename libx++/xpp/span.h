/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * span.h - Span<T>: a non-owning view over a contiguous sequence of T.
 *
 * Equivalent to Rust's &[T] / C++20's std::span. Span<T> for mutable
 * access, Span<const T> for read-only. Trivially copyable, zero-overhead.
 *
 * sizeof(Span<T>) == sizeof(T*) + sizeof(size_t)
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_SPAN_H
#define XPP_SPAN_H

#include <xpp/panic.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace xpp {

/**
 * @brief Non-owning view over a contiguous sequence of T.
 *
 * Span does not manage lifetime — it is the caller's responsibility to
 * ensure the pointed-to memory outlives the Span. Copyable by design:
 * copying a Span just copies the pointer + length (cheap, no allocation).
 *
 * @tparam T  Element type. Use const-qualified T for read-only views
 *            (e.g. Span<const char>).
 */
template <class T> class Span {
public:
  using element_type = T;
  using value_type   = typename std::remove_cv<T>::type;
  using size_type    = size_t;
  using pointer      = T *;
  using reference    = T &;

  /** @brief Sentinel for subspan "until the end". */
  static constexpr size_t npos = static_cast<size_t>(-1);

  /* ── Construction ──────────────────────────────────────────────── */

  /** @brief Empty span. data() == nullptr, size() == 0. */
  constexpr Span() noexcept : m_data(nullptr), m_size(0) {}

  /** @brief Construct from pointer + length. */
  constexpr Span(T *data, size_t len) noexcept : m_data(data), m_size(len) {}

  /** @brief Construct from a C array. */
  template <size_t N> constexpr Span(T (&arr)[N]) noexcept : m_data(arr), m_size(N) {}

  Span(const Span &)            = default;
  Span &operator=(const Span &) = default;
  ~Span()                       = default;

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief Pointer to the first element (nullptr if empty). */
  constexpr T *data() const noexcept {
    return m_data;
  }

  /** @brief Number of elements. */
  constexpr size_t size() const noexcept {
    return m_size;
  }

  /** @brief Size in bytes: size() * sizeof(T). */
  constexpr size_t size_bytes() const noexcept {
    return m_size * sizeof(T);
  }

  /** @brief True if size() == 0. */
  constexpr bool is_empty() const noexcept {
    return m_size == 0;
  }

  /** @brief STL-compatible alias for is_empty(). */
  constexpr bool empty() const noexcept {
    return m_size == 0;
  }

  /* ── Element access ────────────────────────────────────────────── */

  /**
   * @brief Access element at @p idx. Debug-checked bounds.
   */
  T &operator[](size_t idx) const {
    XPP_DEBUG_ASSERT(idx < m_size, "Span::operator[]: index %zu out of range (size %zu)", idx,
                     m_size);
    return m_data[idx];
  }

  /** @brief First element. Debug-asserts non-empty. */
  T &front() const {
    XPP_DEBUG_ASSERT(m_size > 0, "Span::front() on empty span");
    return m_data[0];
  }

  /** @brief Last element. Debug-asserts non-empty. */
  T &back() const {
    XPP_DEBUG_ASSERT(m_size > 0, "Span::back() on empty span");
    return m_data[m_size - 1];
  }

  /* ── Subspans ──────────────────────────────────────────────────── */

  /** @brief First @p count elements. */
  Span first(size_t count) const {
    XPP_DEBUG_ASSERT(count <= m_size, "Span::first(%zu): exceeds size %zu", count, m_size);
    return Span(m_data, count);
  }

  /** @brief Last @p count elements. */
  Span last(size_t count) const {
    XPP_DEBUG_ASSERT(count <= m_size, "Span::last(%zu): exceeds size %zu", count, m_size);
    return Span(m_data + (m_size - count), count);
  }

  /**
   * @brief Sub-range starting at @p offset.
   *
   * @param offset  Starting index.
   * @param count   Number of elements (npos = rest of span).
   */
  Span subspan(size_t offset, size_t count = npos) const {
    XPP_DEBUG_ASSERT(offset <= m_size, "Span::subspan: offset %zu exceeds size %zu", offset,
                     m_size);
    size_t remaining = m_size - offset;
    size_t actual    = (count == npos || count > remaining) ? remaining : count;
    XPP_DEBUG_ASSERT(count == npos || count <= remaining,
                     "Span::subspan: offset %zu + count %zu exceeds size %zu", offset, count,
                     m_size);
    return Span(m_data + offset, actual);
  }

  /* ── Iterators (raw pointers) ──────────────────────────────────── */

  constexpr T *begin() const noexcept {
    return m_data;
  }
  constexpr T *end() const noexcept {
    return m_data + m_size;
  }

  /* ── Conversion ────────────────────────────────────────────────── */

  /**
   * @brief Implicit conversion: Span<T> → Span<const T>.
   *
   * Only participates in overload resolution when T is non-const,
   * preventing a self-conversion warning for Span<const T>.
   */
  template <class U = T, typename std::enable_if<!std::is_const<U>::value, int>::type = 0>
  operator Span<const U>() const noexcept {
    return Span<const U>(m_data, m_size);
  }

private:
  T     *m_data;
  size_t m_size;
};

/* ── Comparison (element-wise) ───────────────────────────────────── */

template <class T> bool operator==(Span<T> a, Span<T> b) {
  if (a.size() != b.size()) return false;
  if (a.data() == b.data()) return true;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) return false;
  }
  return true;
}

template <class T> bool operator!=(Span<T> a, Span<T> b) {
  return !(a == b);
}

/* ── Compile-time size guarantees ────────────────────────────────── */

static_assert(sizeof(Span<int>) == sizeof(int *) + sizeof(size_t),
              "Span<T> must be pointer + size, no overhead");

} // namespace xpp

#endif // XPP_SPAN_H
