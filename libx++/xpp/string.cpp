/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string.cpp - xpp::String implementation
 */

#include <xpp/string.h>

#include <cstring>

#include <x/base/string.h>
#include <x/base/utf8.h>

namespace xpp {

/* ── Construction ──────────────────────────────────────────────────── */

String::String() : m_inner(nullptr) {}

String::String(const char *s) : m_inner(nullptr) {
  size_t n = s ? std::strlen(s) : 0;
  XPP_ASSERT(!s || xValidateUtf8(s, n), "String: input is not valid UTF-8");
  m_inner = s ? xStringCreate(s) : xStringCreate("");
}

String::String(const char *data, size_t len) : m_inner(nullptr) {
  XPP_ASSERT(!data || xValidateUtf8(data, len), "String: input is not valid UTF-8");
  m_inner = xStringCreateLen(data ? data : "", len);
}

Result<String, Utf8Error> String::from(Span<const char> bytes) {
  size_t valid = xUtf8ValidPrefix(bytes.data(), bytes.size());
  if (valid != bytes.size()) {
    return Result<String, Utf8Error>(err, Utf8Error{valid});
  }
  return Result<String, Utf8Error>(ok, String(xStringCreateLen(bytes.data(), bytes.size()), RawTag{}));
}

String String::from_unchecked(const char *s) noexcept {
  return String(xStringCreate(s ? s : ""), RawTag{});
}

String String::from_unchecked(const char *data, size_t len) noexcept {
  return String(xStringCreateLen(data ? data : "", len), RawTag{});
}

String String::from_unchecked(xString raw) noexcept {
  return String(raw ? raw : xStringCreate(""), RawTag{});
}

String String::from_raw(xString raw) noexcept {
  return from_unchecked(raw);
}

String::~String() {
  xStringDestroy(m_inner);
}

/* ── Move ──────────────────────────────────────────────────────────── */

String::String(String &&o) noexcept : m_inner(o.m_inner) {
  o.m_inner = nullptr;
}

String &String::operator=(String &&o) noexcept {
  if (this != &o) {
    xStringDestroy(m_inner);
    m_inner   = o.m_inner;
    o.m_inner = nullptr;
  }
  return *this;
}

/* ── Clone ─────────────────────────────────────────────────────────── */

String String::clone() const {
  return String(m_inner ? xStringDup(m_inner) : xStringCreate(""), RawTag{});
}

/* ── Accessors ─────────────────────────────────────────────────────── */

const char *String::c_str() const noexcept {
  return m_inner ? m_inner : "";
}

size_t String::len() const noexcept {
  return m_inner ? xStringLen(m_inner) : 0;
}

size_t String::capacity() const noexcept {
  return m_inner ? xStringCap(m_inner) : 0;
}

bool String::is_empty() const noexcept {
  return len() == 0;
}

bool String::empty() const noexcept {
  return len() == 0;
}

/* ── Borrowed view ─────────────────────────────────────────────────── */

Span<const char> String::as_span() const noexcept {
  return Span<const char>(c_str(), len());
}

/* ── Mutation ──────────────────────────────────────────────────────── */

void String::append(const String &other) {
  if (other.is_empty()) return;
  ensure_inner();
  xStringAppendLen(&m_inner, other.c_str(), other.len());
}

void String::append(const char *s) {
  if (!s || *s == '\0') return;
  size_t n = std::strlen(s);
  XPP_ASSERT(xValidateUtf8(s, n), "String::append: input is not valid UTF-8");
  ensure_inner();
  xStringAppend(&m_inner, s);
}

void String::append(const char *data, size_t n) {
  if (!data || n == 0) return;
  XPP_ASSERT(xValidateUtf8(data, n), "String::append: input is not valid UTF-8");
  ensure_inner();
  xStringAppendLen(&m_inner, data, n);
}

Result<bool, Utf8Error> String::try_append(Span<const char> bytes) {
  if (bytes.is_empty()) return Result<bool, Utf8Error>(ok, true);
  size_t valid = xUtf8ValidPrefix(bytes.data(), bytes.size());
  if (valid != bytes.size()) {
    return Result<bool, Utf8Error>(err, Utf8Error{valid});
  }
  ensure_inner();
  xStringAppendLen(&m_inner, bytes.data(), bytes.size());
  return Result<bool, Utf8Error>(ok, true);
}

void String::clear() {
  if (m_inner) xStringClear(m_inner);
}

void String::truncate(size_t byte_len) {
  size_t cur = len();
  XPP_DEBUG_ASSERT(byte_len <= cur, "String::truncate(%zu): exceeds len %zu", byte_len, cur);
  if (byte_len == cur) return;
  XPP_DEBUG_ASSERT(byte_len == 0 || (m_inner[byte_len] & 0xC0) != 0x80,
                   "String::truncate(%zu): not on UTF-8 character boundary", byte_len);
  xStringTruncate(m_inner, byte_len);
}

void String::reserve(size_t additional) {
  ensure_inner();
  m_inner = xStringGrow(m_inner, additional);
}

void String::shrink_to_fit() {
  if (m_inner) m_inner = xStringShrinkToFit(m_inner);
}

/* ── Search ────────────────────────────────────────────────────────── */

Option<size_t> String::find(const char *needle) const {
  if (!m_inner || !needle) return none;
  size_t pos = xStringFindStr(m_inner, needle);
  return pos == XSTRING_NONE ? Option<size_t>(none) : Option<size_t>(pos);
}

Option<size_t> String::find(Span<const char> needle) const {
  if (!m_inner || needle.is_empty()) return none;
  size_t pos = xStringFind(m_inner, needle.data(), needle.size());
  return pos == XSTRING_NONE ? Option<size_t>(none) : Option<size_t>(pos);
}

/* ── Comparison ────────────────────────────────────────────────────── */

bool operator==(const String &a, const String &b) {
  return xStringEq(a.m_inner, b.m_inner);
}

bool operator!=(const String &a, const String &b) {
  return !xStringEq(a.m_inner, b.m_inner);
}

bool operator==(const String &a, const char *b) {
  const char *ac = a.c_str();
  return b ? (std::strcmp(ac, b) == 0) : (*ac == '\0');
}

bool operator==(const char *a, const String &b) {
  return b == a;
}

bool operator!=(const String &a, const char *b) {
  return !(a == b);
}

bool operator!=(const char *a, const String &b) {
  return !(b == a);
}

/* ── Concatenation sugar ───────────────────────────────────────────── */

String &String::operator+=(const char *s) {
  append(s);
  return *this;
}

String &String::operator+=(const String &other) {
  append(other);
  return *this;
}

/* ── C interop ─────────────────────────────────────────────────────── */

xString String::release() noexcept {
  xString raw = m_inner;
  m_inner     = nullptr;
  return raw;
}

/* ── Private ───────────────────────────────────────────────────────── */

void String::ensure_inner() {
  if (!m_inner) m_inner = xStringCreate("");
}

} // namespace xpp
