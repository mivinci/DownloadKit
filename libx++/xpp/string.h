/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string.h - String: an owned, UTF-8-guaranteed byte string.
 *
 * Invariant: the internal buffer is always valid UTF-8. Construction
 * from untrusted data validates; mutation methods preserve validity.
 * Move-only with explicit clone() — same ownership semantics as
 * Rust's String.
 *
 * Internally backed by Vec<char> (same layout as Rust's String which
 * is a newtype over Vec<u8>). The UTF-8 contract is enforced at the
 * xpp::String API boundary; Vec<char> handles all memory management.
 *
 * C++11-compatible.
 */

#ifndef XPP_STRING_H
#define XPP_STRING_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/span.h>
#include <xpp/vec.h>

#include <cstddef>

namespace xpp {

/**
 * @brief Error returned when a byte sequence is not valid UTF-8.
 *
 * Carries the byte offset of the first invalid byte, enabling
 * callers to report position or extract the valid prefix.
 */
struct Utf8Error {
  /** Byte offset of the first invalid byte (0..input_len). */
  size_t valid_up_to;
};

/**
 * @brief Owned, UTF-8-guaranteed byte string.
 *
 * All constructors and mutators enforce or assume valid UTF-8.
 * Use as_span() for a borrowed view; use Span<const char> as the
 * general "string slice" type in function signatures.
 *
 * Move-only. Use clone() for explicit deep copies.
 */
class String {
public:
  /* ── Construction ──────────────────────────────────────────────── */

  /**
   * @brief Construct an empty string. No heap allocation.
   *
   * @code
   *   String s;
   *   assert(s.is_empty() && s.len() == 0);
   * @endcode
   */
  String() noexcept;

  /**
   * @brief Construct from a NUL-terminated C string.
   *
   * Validates UTF-8 at runtime. Panics if @p s is not valid UTF-8.
   * Use for string literals and other trusted sources.
   *
   * @param s  NUL-terminated C string to copy (nullptr → empty string).
   *
   * @code
   *   String greeting("hello 世界");
   * @endcode
   */
  explicit String(const char *s);

  /**
   * @brief Construct from raw bytes with explicit length.
   *
   * Validates UTF-8. Panics if the data is not valid.
   *
   * @param data  Pointer to UTF-8 bytes (nullptr → empty string).
   * @param len   Number of bytes to copy.
   *
   * @code
   *   const char buf[] = "hello\x00world";
   *   String s(buf, 5);  // "hello" (binary-safe length)
   * @endcode
   */
  String(const char *data, size_t len);

  /**
   * @brief Fallible construction from untrusted bytes.
   *
   * Validates the entire buffer. On failure, reports the byte offset
   * of the first invalid sequence.
   *
   * @param bytes  Byte span to validate and copy.
   * @return       Ok(String) if valid, Err(Utf8Error{valid_up_to}) otherwise.
   *
   * @code
   *   auto r = String::from(network_data);
   *   if (r.is_err()) {
   *     log("bad UTF-8 at byte %zu", r.unwrap_err().valid_up_to);
   *   }
   * @endcode
   */
  static Result<String, Utf8Error> from(Span<const char> bytes);

  /**
   * @brief Construct without validation. Caller guarantees UTF-8.
   *
   * Use when data is known-valid (e.g. from a trusted codec or
   * already-validated source). No runtime cost.
   *
   * @param s  NUL-terminated C string (nullptr → empty).
   */
  static String from_unchecked(const char *s) noexcept;

  /**
   * @brief Construct without validation from pointer + length.
   *
   * @param data  Pointer to bytes (nullptr → empty).
   * @param len   Number of bytes.
   */
  static String from_unchecked(const char *data, size_t len) noexcept;

  ~String();

  /* ── Move-only ─────────────────────────────────────────────────── */

  String(String &&o) noexcept;
  String &operator=(String &&o) noexcept;
  String(const String &)            = delete;
  String &operator=(const String &) = delete;

  /* ── Explicit clone ────────────────────────────────────────────── */

  /** @brief Deep-copy this string. */
  String clone() const;

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief NUL-terminated C string. */
  const char *c_str() const noexcept {
    return m_buf.is_empty() ? "" : m_buf.data();
  }

  /** @brief Byte length (not code point count). */
  size_t len() const noexcept { return m_buf.len(); }

  /** @brief Allocated capacity in bytes. */
  size_t capacity() const noexcept { return m_buf.capacity(); }

  /** @brief True if len() == 0. */
  bool is_empty() const noexcept { return m_buf.is_empty(); }

  /** @brief STL-compatible alias for is_empty(). */
  bool empty() const noexcept { return m_buf.is_empty(); }

  /* ── Borrowed view ─────────────────────────────────────────────── */

  /** @brief Non-owning view of the UTF-8 bytes. */
  Span<const char> as_span() const noexcept {
    return Span<const char>(m_buf.data(), m_buf.len());
  }

  /* ── Mutation ──────────────────────────────────────────────────── */

  /**
   * @brief Append another String. No validation needed (already UTF-8).
   *
   * @param other  String to append.
   *
   * @code
   *   String s("hello");
   *   String w(" world");
   *   s.append(w);  // s == "hello world"
   * @endcode
   */
  void append(const String &other);

  /**
   * @brief Append a NUL-terminated C string. Validates; panics if invalid.
   *
   * @param s  C string to append (nullptr/empty → no-op).
   */
  void append(const char *s);

  /**
   * @brief Append raw bytes with explicit length. Validates; panics if invalid.
   *
   * @param data  Pointer to UTF-8 bytes.
   * @param n     Number of bytes to append.
   */
  void append(const char *data, size_t n);

  /**
   * @brief Try to append bytes. Returns error position if not valid UTF-8.
   *
   * On failure, the string is unchanged.
   *
   * @param bytes  Byte span to validate and append.
   * @return       Ok(true) on success, Err(Utf8Error{valid_up_to}) on failure.
   *
   * @code
   *   auto r = s.try_append(untrusted_bytes);
   *   if (r.is_err()) { ... } // string unchanged
   * @endcode
   */
  Result<bool, Utf8Error> try_append(Span<const char> bytes);

  /** @brief Clear to empty. Does not free the allocation. */
  void clear();

  /**
   * @brief Truncate to @p byte_len bytes.
   *
   * Debug-asserts that byte_len <= len() and falls on a UTF-8 char boundary.
   *
   * @param byte_len  Target byte length. Must be on a character boundary.
   *
   * @code
   *   String s("hello");
   *   s.truncate(3);  // s == "hel"
   * @endcode
   */
  void truncate(size_t byte_len);

  /**
   * @brief Pre-allocate space for at least @p additional more bytes.
   *
   * @param additional  Extra byte slots to reserve beyond current len().
   */
  void reserve(size_t additional);

  /** @brief Shrink allocation to fit current content. */
  void shrink_to_fit();

  /* ── Search ────────────────────────────────────────────────────── */

  /**
   * @brief Find first occurrence of a NUL-terminated needle.
   *
   * @param needle  C string to search for.
   * @return        Some(byte_offset) if found, None otherwise.
   *
   * @code
   *   String s("hello world");
   *   auto pos = s.find("world");  // Some(6)
   * @endcode
   */
  Option<size_t> find(const char *needle) const;

  /**
   * @brief Find first occurrence of a byte-span needle.
   *
   * @param needle  Span of bytes to search for.
   * @return        Some(byte_offset) if found, None otherwise.
   */
  Option<size_t> find(Span<const char> needle) const;

  /* ── Comparison ────────────────────────────────────────────────── */

  friend bool operator==(const String &a, const String &b);
  friend bool operator!=(const String &a, const String &b);
  friend bool operator==(const String &a, const char *b);
  friend bool operator==(const char *a, const String &b);
  friend bool operator!=(const String &a, const char *b);
  friend bool operator!=(const char *a, const String &b);

  /* ── Concatenation sugar ───────────────────────────────────────── */

  String &operator+=(const char *s);
  String &operator+=(const String &other);

  /* ── Access to underlying buffer ───────────────────────────────── */

  /** @brief Get a reference to the underlying Vec<char>. */
  const Vec<char> &as_vec() const noexcept;

  /**
   * @brief Consume this String and return the underlying Vec<char>.
   *
   * The String is left in a moved-from state.
   */
  Vec<char> into_vec() noexcept;

private:
  Vec<char> m_buf;

  /** @brief Private: construct directly from a Vec<char>. */
  explicit String(Vec<char> &&buf) noexcept;

  /** @brief Ensure a NUL byte exists after len() for c_str(). */
  void ensure_nul();
};

} // namespace xpp

#endif // XPP_STRING_H
