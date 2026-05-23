/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string.cpp - xpp::String implementation
 */

#include <xpp/string.h>

#include <cstring>

extern "C" {
#include <x/base/compat.h>
#include <x/base/utf8.h>
}

namespace xpp {

/* ── Construction ──────────────────────────────────────────────────── */

String::String() noexcept : m_buf() {}

String::String(const char *s) : m_buf() {
  if (!s) return;
  size_t n = std::strlen(s);
  XPP_ASSERT(xValidateUtf8(s, n), "String: input is not valid UTF-8");
  m_buf.reserve(n + 1);
  m_buf.extend(Span<const char>(s, n));
  ensure_nul();
}

String::String(const char *data, size_t len) : m_buf() {
  if (!data || len == 0) return;
  XPP_ASSERT(xValidateUtf8(data, len), "String: input is not valid UTF-8");
  m_buf.reserve(len + 1);
  m_buf.extend(Span<const char>(data, len));
  ensure_nul();
}

Result<String, Utf8Error> String::from(Span<const char> bytes) {
  size_t valid = xUtf8ValidPrefix(bytes.data(), bytes.size());
  if (valid != bytes.size()) {
    return Result<String, Utf8Error>(err, Utf8Error{valid});
  }
  Vec<char> buf;
  buf.reserve(bytes.size() + 1);
  buf.extend(bytes);
  return Result<String, Utf8Error>(ok, String(std::move(buf)));
}

String String::from_unchecked(const char *s) noexcept {
  if (!s) return String();
  size_t n = std::strlen(s);
  Vec<char> buf;
  buf.reserve(n + 1);
  buf.extend(Span<const char>(s, n));
  return String(std::move(buf));
}

String String::from_unchecked(const char *data, size_t len) noexcept {
  if (!data || len == 0) return String();
  Vec<char> buf;
  buf.reserve(len + 1);
  buf.extend(Span<const char>(data, len));
  return String(std::move(buf));
}

String::~String() = default;

/* ── Move ──────────────────────────────────────────────────────────── */

String::String(String &&o) noexcept : m_buf(std::move(o.m_buf)) {}

String &String::operator=(String &&o) noexcept {
  m_buf = std::move(o.m_buf);
  return *this;
}

/* ── Clone ─────────────────────────────────────────────────────────── */

String String::clone() const {
  return String(m_buf.clone());
}

/* ── Mutation ──────────────────────────────────────────────────────── */

void String::append(const String &other) {
  if (other.is_empty()) return;
  m_buf.extend(other.as_span());
  ensure_nul();
}

void String::append(const char *s) {
  if (!s || *s == '\0') return;
  size_t n = std::strlen(s);
  XPP_ASSERT(xValidateUtf8(s, n), "String::append: input is not valid UTF-8");
  m_buf.extend(Span<const char>(s, n));
  ensure_nul();
}

void String::append(const char *data, size_t n) {
  if (!data || n == 0) return;
  XPP_ASSERT(xValidateUtf8(data, n), "String::append: input is not valid UTF-8");
  m_buf.extend(Span<const char>(data, n));
  ensure_nul();
}

Result<bool, Utf8Error> String::try_append(Span<const char> bytes) {
  if (bytes.is_empty()) return Result<bool, Utf8Error>(ok, true);
  size_t valid = xUtf8ValidPrefix(bytes.data(), bytes.size());
  if (valid != bytes.size()) {
    return Result<bool, Utf8Error>(err, Utf8Error{valid});
  }
  m_buf.extend(bytes);
  ensure_nul();
  return Result<bool, Utf8Error>(ok, true);
}

void String::clear() {
  m_buf.clear();
}

void String::truncate(size_t byte_len) {
  size_t cur = m_buf.len();
  XPP_DEBUG_ASSERT(byte_len <= cur, "String::truncate(%zu): exceeds len %zu", byte_len, cur);
  if (byte_len == cur) return;
  XPP_DEBUG_ASSERT(byte_len == 0 || (m_buf[byte_len] & 0xC0) != 0x80,
                   "String::truncate(%zu): not on UTF-8 character boundary", byte_len);
  m_buf.truncate(byte_len);
  ensure_nul();
}

void String::reserve(size_t additional) {
  m_buf.reserve(additional + 1); /* +1 for NUL */
}

void String::shrink_to_fit() {
  m_buf.shrink_to_fit();
}

/* ── Search ────────────────────────────────────────────────────────── */

/** Threshold below which naive memcmp scan beats memmem call overhead. */
static constexpr size_t kFindThreshold = 32;

static Option<size_t> find_impl(const char *haystack, size_t hlen,
                                const char *needle, size_t nlen) {
  if (nlen > hlen) return none;
  if (nlen < kFindThreshold) {
    size_t last = hlen - nlen;
    for (size_t i = 0; i <= last; ++i) {
      if (std::memcmp(haystack + i, needle, nlen) == 0) {
        return Option<size_t>(i);
      }
    }
    return none;
  }
  const void *found = memmem(haystack, hlen, needle, nlen);
  return found ? Option<size_t>(static_cast<size_t>(
                     static_cast<const char *>(found) - haystack))
               : Option<size_t>(none);
}

Option<size_t> String::find(const char *needle) const {
  if (m_buf.is_empty() || !needle || *needle == '\0') return none;
  return find_impl(m_buf.data(), m_buf.len(), needle, std::strlen(needle));
}

Option<size_t> String::find(Span<const char> needle) const {
  if (m_buf.is_empty() || needle.is_empty()) return none;
  return find_impl(m_buf.data(), m_buf.len(), needle.data(), needle.size());
}

/* ── Comparison ────────────────────────────────────────────────────── */

bool operator==(const String &a, const String &b) {
  if (a.len() != b.len()) return false;
  if (a.len() == 0) return true;
  return std::memcmp(a.m_buf.data(), b.m_buf.data(), a.len()) == 0;
}

bool operator!=(const String &a, const String &b) { return !(a == b); }

bool operator==(const String &a, const char *b) {
  if (!b) return a.is_empty();
  size_t blen = std::strlen(b);
  if (a.len() != blen) return false;
  if (blen == 0) return true;
  return std::memcmp(a.m_buf.data(), b, blen) == 0;
}

bool operator==(const char *a, const String &b) { return b == a; }
bool operator!=(const String &a, const char *b) { return !(a == b); }
bool operator!=(const char *a, const String &b) { return !(b == a); }

/* ── Concatenation sugar ───────────────────────────────────────────── */

String &String::operator+=(const char *s) {
  append(s);
  return *this;
}

String &String::operator+=(const String &other) {
  append(other);
  return *this;
}

/* ── Access to underlying buffer ───────────────────────────────────── */

const Vec<char> &String::as_vec() const noexcept { return m_buf; }

Vec<char> String::into_vec() noexcept { return std::move(m_buf); }

/* ── Private ───────────────────────────────────────────────────────── */

String::String(Vec<char> &&buf) noexcept : m_buf(std::move(buf)) {
  ensure_nul();
}

void String::ensure_nul() {
  if (m_buf.is_empty()) return;
  if (m_buf.capacity() <= m_buf.len()) {
    m_buf.reserve(1);
  }
  m_buf.data()[m_buf.len()] = '\0';
}

} // namespace xpp
