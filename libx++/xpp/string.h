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
 * C++11-compatible. Header-only (inlines into xString + xValidateUtf8).
 */

#ifndef XPP_STRING_H
#define XPP_STRING_H

#include <xpp/option.h>
#include <xpp/panic.h>
#include <xpp/result.h>
#include <xpp/span.h>

#include <cstddef>
#include <cstring>
#include <utility>

#include <x/base/string.h>
#include <x/base/utf8.h>

namespace xpp {

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
  String() : m_inner(xStringCreate("")) {}

  /**
   * @brief Construct from a NUL-terminated C string.
   *
   * Validates UTF-8. Panics if @p s contains invalid sequences.
   * Use for string literals and known-good sources.
   */
  explicit String(const char *s) : m_inner(nullptr) {
    size_t n = s ? std::strlen(s) : 0;
    XPP_ASSERT(!s || xValidateUtf8(s, n), "String: input is not valid UTF-8");
    m_inner = s ? xStringCreate(s) : xStringCreate("");
  }

  /**
   * @brief Construct from raw bytes with explicit length.
   *
   * Validates UTF-8. Panics if the data is invalid.
   */
  String(const char *data, size_t len) : m_inner(nullptr) {
    XPP_ASSERT(!data || xValidateUtf8(data, len), "String: input is not valid UTF-8");
    m_inner = xStringCreateLen(data ? data : "", len);
  }

  /**
   * @brief Fallible construction from untrusted bytes.
   *
   * @return Ok(String) if valid UTF-8, Err(-1) otherwise.
   */
  static Result<String, int> from(Span<const char> bytes) {
    if (!xValidateUtf8(bytes.data(), bytes.size())) {
      return Result<String, int>(err, -1);
    }
    String s;
    xStringDestroy(s.m_inner);
    s.m_inner = xStringCreateLen(bytes.data(), bytes.size());
    return Result<String, int>(ok, std::move(s));
  }

  /**
   * @brief Construct without validation. Caller guarantees UTF-8.
   */
  static String from_unchecked(const char *s) noexcept {
    String r;
    xStringDestroy(r.m_inner);
    r.m_inner = xStringCreate(s ? s : "");
    return r;
  }

  /** @brief Construct without validation from data + length. */
  static String from_unchecked(const char *data, size_t len) noexcept {
    String r;
    xStringDestroy(r.m_inner);
    r.m_inner = xStringCreateLen(data ? data : "", len);
    return r;
  }

  /**
   * @brief Take ownership of a raw xString. Caller guarantees UTF-8.
   *
   * The xString must have been allocated via xStringCreate* family.
   * After this call, the caller must not use or destroy the xString.
   */
  static String from_unchecked(xString raw) noexcept {
    String r;
    xStringDestroy(r.m_inner);
    r.m_inner = raw ? raw : xStringCreate("");
    return r;
  }

  /** @brief Alias for from_unchecked(xString). */
  static String from_raw(xString raw) noexcept {
    return from_unchecked(raw);
  }

  ~String() {
    xStringDestroy(m_inner);
  }

  /* ── Move-only ─────────────────────────────────────────────────── */

  String(String &&o) noexcept : m_inner(o.m_inner) {
    o.m_inner = nullptr;
  }

  String &operator=(String &&o) noexcept {
    if (this != &o) {
      xStringDestroy(m_inner);
      m_inner   = o.m_inner;
      o.m_inner = nullptr;
    }
    return *this;
  }

  String(const String &)            = delete;
  String &operator=(const String &) = delete;

  /* ── Explicit clone ────────────────────────────────────────────── */

  /** @brief Deep-copy this string. */
  String clone() const {
    String r;
    xStringDestroy(r.m_inner);
    r.m_inner = m_inner ? xStringDup(m_inner) : xStringCreate("");
    return r;
  }

  /* ── Accessors ─────────────────────────────────────────────────── */

  /** @brief NUL-terminated C string. Empty string if moved-from. */
  const char *c_str() const noexcept {
    return m_inner ? m_inner : "";
  }

  /** @brief Byte length (not code point count). */
  size_t len() const noexcept {
    return m_inner ? xStringLen(m_inner) : 0;
  }

  /** @brief Allocated capacity in bytes. */
  size_t capacity() const noexcept {
    return m_inner ? xStringCap(m_inner) : 0;
  }

  /** @brief True if len() == 0. */
  bool is_empty() const noexcept {
    return len() == 0;
  }

  /** @brief STL-compatible alias for is_empty(). */
  bool empty() const noexcept {
    return len() == 0;
  }

  /* ── Borrowed view ─────────────────────────────────────────────── */

  /** @brief Non-owning view of the UTF-8 bytes. */
  Span<const char> as_span() const noexcept {
    return Span<const char>(c_str(), len());
  }

  /* ── Mutation ──────────────────────────────────────────────────── */

  /** @brief Append another String (already valid, no check needed). */
  void append(const String &other) {
    if (other.is_empty()) return;
    ensure_inner();
    xStringAppendLen(&m_inner, other.c_str(), other.len());
  }

  /**
   * @brief Append a NUL-terminated C string. Validates; panics if invalid.
   */
  void append(const char *s) {
    if (!s || *s == '\0') return;
    size_t n = std::strlen(s);
    XPP_ASSERT(xValidateUtf8(s, n), "String::append: input is not valid UTF-8");
    ensure_inner();
    xStringAppend(&m_inner, s);
  }

  /**
   * @brief Append raw bytes with length. Validates; panics if invalid.
   */
  void append(const char *data, size_t n) {
    if (!data || n == 0) return;
    XPP_ASSERT(xValidateUtf8(data, n), "String::append: input is not valid UTF-8");
    ensure_inner();
    xStringAppendLen(&m_inner, data, n);
  }

  /**
   * @brief Try to append bytes. Returns Err(-1) if not valid UTF-8.
   */
  Result<bool, int> try_append(Span<const char> bytes) {
    if (bytes.is_empty()) return Result<bool, int>(ok, true);
    if (!xValidateUtf8(bytes.data(), bytes.size())) {
      return Result<bool, int>(err, -1);
    }
    ensure_inner();
    xStringAppendLen(&m_inner, bytes.data(), bytes.size());
    return Result<bool, int>(ok, true);
  }

  /** @brief Clear to empty string (does not free allocation). */
  void clear() {
    if (m_inner) xStringClear(m_inner);
  }

  /**
   * @brief Truncate to @p byte_len bytes.
   *
   * Debug-asserts that byte_len falls on a UTF-8 character boundary.
   */
  void truncate(size_t byte_len) {
    size_t cur = len();
    XPP_DEBUG_ASSERT(byte_len <= cur, "String::truncate(%zu): exceeds len %zu", byte_len, cur);
    if (byte_len == cur) return;
    XPP_DEBUG_ASSERT(byte_len == 0 || (m_inner[byte_len] & 0xC0) != 0x80,
                     "String::truncate(%zu): not on UTF-8 character boundary", byte_len);
    xStringTruncate(m_inner, byte_len);
  }

  /** @brief Pre-allocate space for at least @p additional more bytes. */
  void reserve(size_t additional) {
    ensure_inner();
    m_inner = xStringGrow(m_inner, additional);
  }

  /** @brief Shrink allocation to fit current content. */
  void shrink_to_fit() {
    if (m_inner) m_inner = xStringShrinkToFit(m_inner);
  }

  /* ── Search ────────────────────────────────────────────────────── */

  /** @brief Find first occurrence of NUL-terminated needle. */
  Option<size_t> find(const char *needle) const {
    if (!m_inner || !needle) return none;
    size_t pos = xStringFindStr(m_inner, needle);
    return pos == XSTRING_NONE ? Option<size_t>(none) : Option<size_t>(pos);
  }

  /** @brief Find first occurrence of a byte span as needle. */
  Option<size_t> find(Span<const char> needle) const {
    if (!m_inner || needle.is_empty()) return none;
    size_t pos = xStringFind(m_inner, needle.data(), needle.size());
    return pos == XSTRING_NONE ? Option<size_t>(none) : Option<size_t>(pos);
  }

  /* ── Comparison ────────────────────────────────────────────────── */

  friend bool operator==(const String &a, const String &b) {
    return xStringEq(a.m_inner, b.m_inner);
  }
  friend bool operator!=(const String &a, const String &b) {
    return !xStringEq(a.m_inner, b.m_inner);
  }
  friend bool operator==(const String &a, const char *b) {
    const char *ac = a.c_str();
    return b ? (std::strcmp(ac, b) == 0) : (*ac == '\0');
  }
  friend bool operator==(const char *a, const String &b) {
    return b == a;
  }
  friend bool operator!=(const String &a, const char *b) {
    return !(a == b);
  }
  friend bool operator!=(const char *a, const String &b) {
    return !(b == a);
  }

  /* ── Concatenation sugar ───────────────────────────────────────── */

  String &operator+=(const char *s) {
    append(s);
    return *this;
  }

  String &operator+=(const String &other) {
    append(other);
    return *this;
  }

  /* ── C interop ─────────────────────────────────────────────────── */

  /**
   * @brief Release ownership of the internal xString.
   *
   * After this call, the String is in a moved-from state. The caller
   * is responsible for calling xStringDestroy on the returned value.
   */
  xString release() noexcept {
    xString raw = m_inner;
    m_inner     = nullptr;
    return raw;
  }

private:
  xString m_inner;

  /** @brief Ensure m_inner is non-null (recover from moved-from state). */
  void ensure_inner() {
    if (!m_inner) m_inner = xStringCreate("");
  }
};

} // namespace xpp

#endif // XPP_STRING_H
