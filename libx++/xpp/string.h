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
 * Internally wraps xString (libx SDS) for allocation/growth. The
 * UTF-8 contract is enforced at the xpp::String API boundary.
 *
 * C++11-compatible.
 */

#ifndef XPP_STRING_H
#define XPP_STRING_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/span.h>

#include <cstddef>

#include <x/base/string.h>

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

  /** @brief Empty string "". Always valid. */
  String();

  /**
   * @brief Construct from a NUL-terminated C string.
   *
   * Validates UTF-8. Panics if @p s contains invalid sequences.
   * Use for string literals and known-good sources.
   */
  explicit String(const char *s);

  /**
   * @brief Construct from raw bytes with explicit length.
   *
   * Validates UTF-8. Panics if the data is invalid.
   */
  String(const char *data, size_t len);

  /**
   * @brief Fallible construction from untrusted bytes.
   *
   * @return Ok(String) if valid UTF-8, Err(-1) otherwise.
   */
  static Result<String, Utf8Error> from(Span<const char> bytes);

  /**
   * @brief Construct without validation. Caller guarantees UTF-8.
   */
  static String from_unchecked(const char *s) noexcept;

  /** @brief Construct without validation from data + length. */
  static String from_unchecked(const char *data, size_t len) noexcept;

  /**
   * @brief Take ownership of a raw xString. Caller guarantees UTF-8.
   *
   * The xString must have been allocated via xStringCreate* family.
   * After this call, the caller must not use or destroy the xString.
   */
  static String from_unchecked(xString raw) noexcept;

  /** @brief Alias for from_unchecked(xString). */
  static String from_raw(xString raw) noexcept;

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

  /** @brief NUL-terminated C string. Empty string if moved-from. */
  const char *c_str() const noexcept;

  /** @brief Byte length (not code point count). */
  size_t len() const noexcept;

  /** @brief Allocated capacity in bytes. */
  size_t capacity() const noexcept;

  /** @brief True if len() == 0. */
  bool is_empty() const noexcept;

  /** @brief STL-compatible alias for is_empty(). */
  bool empty() const noexcept;

  /* ── Borrowed view ─────────────────────────────────────────────── */

  /** @brief Non-owning view of the UTF-8 bytes. */
  Span<const char> as_span() const noexcept;

  /* ── Mutation ──────────────────────────────────────────────────── */

  /** @brief Append another String (already valid, no check needed). */
  void append(const String &other);

  /** @brief Append a NUL-terminated C string. Validates; panics if invalid. */
  void append(const char *s);

  /** @brief Append raw bytes with length. Validates; panics if invalid. */
  void append(const char *data, size_t n);

  /** @brief Try to append bytes. Returns Err with position if not valid UTF-8. */
  Result<bool, Utf8Error> try_append(Span<const char> bytes);

  /** @brief Clear to empty string (does not free allocation). */
  void clear();

  /**
   * @brief Truncate to @p byte_len bytes.
   *
   * Debug-asserts that byte_len falls on a UTF-8 character boundary.
   */
  void truncate(size_t byte_len);

  /** @brief Pre-allocate space for at least @p additional more bytes. */
  void reserve(size_t additional);

  /** @brief Shrink allocation to fit current content. */
  void shrink_to_fit();

  /* ── Search ────────────────────────────────────────────────────── */

  /** @brief Find first occurrence of NUL-terminated needle. */
  Option<size_t> find(const char *needle) const;

  /** @brief Find first occurrence of a byte span as needle. */
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

  /* ── C interop ─────────────────────────────────────────────────── */

  /**
   * @brief Release ownership of the internal xString.
   *
   * After this call, the String is in a moved-from state. The caller
   * is responsible for calling xStringDestroy on the returned value.
   */
  xString release() noexcept;

private:
  xString m_inner;

  void ensure_inner();

  /** @brief Private tag for direct-init from raw xString (no allocation). */
  struct RawTag {};
  String(xString raw, RawTag) noexcept : m_inner(raw) {}
};

} // namespace xpp

#endif // XPP_STRING_H
