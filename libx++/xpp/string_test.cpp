/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * string_test.cpp - Unit tests for xpp::String
 */

#include <xpp/string.h>

#include <gtest/gtest.h>

#include <cstring>
#include <utility>

using xpp::None;
using xpp::none;
using xpp::Option;
using xpp::Result;
using xpp::Span;
using xpp::String;

/* ── Construction ──────────────────────────────────────────────────── */

TEST(StringTest, DefaultConstruction) {
  String s;
  EXPECT_EQ(s.len(), 0u);
  EXPECT_TRUE(s.is_empty());
  EXPECT_STREQ(s.c_str(), "");
}

TEST(StringTest, FromCString) {
  String s("hello");
  EXPECT_EQ(s.len(), 5u);
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, FromDataAndLen) {
  String s("hello world", 5);
  EXPECT_EQ(s.len(), 5u);
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, FromNullptr) {
  String s(nullptr);
  EXPECT_EQ(s.len(), 0u);
  EXPECT_TRUE(s.is_empty());
}

TEST(StringTest, FromUtf8Multibyte) {
  /* "中文" = 6 bytes UTF-8 */
  String s("\xE4\xB8\xAD\xE6\x96\x87");
  EXPECT_EQ(s.len(), 6u);
}

TEST(StringTest, FromEmoji) {
  /* "😀" = 4 bytes UTF-8 */
  String s("\xF0\x9F\x98\x80");
  EXPECT_EQ(s.len(), 4u);
}

/* ── Fallible construction ─────────────────────────────────────────── */

TEST(StringTest, FromValidBytes) {
  const char data[] = "valid utf8";
  auto r = String::from(Span<const char>(data, 10));
  EXPECT_TRUE(r.is_ok());
  EXPECT_STREQ(r.unwrap().c_str(), "valid utf8");
}

TEST(StringTest, FromInvalidBytes) {
  const char data[] = "\xC0\x80";  /* overlong */
  auto r = String::from(Span<const char>(data, 2));
  EXPECT_TRUE(r.is_err());
}

TEST(StringTest, FromEmptyBytes) {
  auto r = String::from(Span<const char>());
  EXPECT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is_empty());
}

/* ── Unchecked construction ────────────────────────────────────────── */

TEST(StringTest, FromUncheckedCStr) {
  auto s = String::from_unchecked("test");
  EXPECT_STREQ(s.c_str(), "test");
}

TEST(StringTest, FromUncheckedDataLen) {
  auto s = String::from_unchecked("hello", 3);
  EXPECT_EQ(s.len(), 3u);
  EXPECT_STREQ(s.c_str(), "hel");
}

TEST(StringTest, FromRawXString) {
  xString raw = xStringCreate("owned");
  auto s      = String::from_raw(raw);
  EXPECT_STREQ(s.c_str(), "owned");
  /* raw is now owned by s — no double free */
}

/* ── Move semantics ────────────────────────────────────────────────── */

TEST(StringTest, MoveConstruction) {
  String a("hello");
  String b(std::move(a));
  EXPECT_STREQ(b.c_str(), "hello");
  EXPECT_EQ(b.len(), 5u);
  /* a is moved-from */
  EXPECT_TRUE(a.is_empty());
  EXPECT_STREQ(a.c_str(), "");
}

TEST(StringTest, MoveAssignment) {
  String a("hello");
  String b("world");
  b = std::move(a);
  EXPECT_STREQ(b.c_str(), "hello");
  EXPECT_TRUE(a.is_empty());
}

TEST(StringTest, MovedFromState) {
  String a("test");
  String b(std::move(a));
  /* moved-from is safe to use: empty, no crash */
  EXPECT_EQ(a.len(), 0u);
  EXPECT_EQ(a.capacity(), 0u);
  EXPECT_TRUE(a.is_empty());
  EXPECT_TRUE(a.empty());
  EXPECT_STREQ(a.c_str(), "");
}

/* ── Clone ─────────────────────────────────────────────────────────── */

TEST(StringTest, Clone) {
  String a("hello");
  String b = a.clone();
  EXPECT_STREQ(b.c_str(), "hello");
  EXPECT_EQ(b.len(), 5u);
  /* Independent: modifying one doesn't affect the other */
  EXPECT_STREQ(a.c_str(), "hello");
}

/* ── Accessors ─────────────────────────────────────────────────────── */

TEST(StringTest, CStr) {
  String s("test");
  EXPECT_STREQ(s.c_str(), "test");
}

TEST(StringTest, Len) {
  String s("hello");
  EXPECT_EQ(s.len(), 5u);
}

TEST(StringTest, Capacity) {
  String s("hi");
  EXPECT_GE(s.capacity(), 2u);
}

TEST(StringTest, IsEmpty) {
  String empty;
  String nonempty("x");
  EXPECT_TRUE(empty.is_empty());
  EXPECT_FALSE(nonempty.is_empty());
}

TEST(StringTest, Empty) {
  String empty;
  String nonempty("x");
  EXPECT_TRUE(empty.empty());
  EXPECT_FALSE(nonempty.empty());
}

/* ── as_span ───────────────────────────────────────────────────────── */

TEST(StringTest, AsSpan) {
  String s("hello");
  Span<const char> sp = s.as_span();
  EXPECT_EQ(sp.data(), s.c_str());
  EXPECT_EQ(sp.size(), 5u);
}

TEST(StringTest, AsSpanEmpty) {
  String s;
  Span<const char> sp = s.as_span();
  EXPECT_EQ(sp.size(), 0u);
}

/* ── Append ────────────────────────────────────────────────────────── */

TEST(StringTest, AppendString) {
  String a("hello");
  String b(" world");
  a.append(b);
  EXPECT_STREQ(a.c_str(), "hello world");
}

TEST(StringTest, AppendCStr) {
  String s("hello");
  s.append(" world");
  EXPECT_STREQ(s.c_str(), "hello world");
}

TEST(StringTest, AppendDataLen) {
  String s("hi");
  s.append(" there!", 6);
  EXPECT_STREQ(s.c_str(), "hi there");
}

TEST(StringTest, AppendEmpty) {
  String s("hello");
  s.append("");
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, AppendToMovedFrom) {
  String a("test");
  String b(std::move(a));
  /* appending to moved-from should work (recovers to empty first) */
  a.append("recovered");
  EXPECT_STREQ(a.c_str(), "recovered");
}

TEST(StringTest, TryAppendValid) {
  String s("hello");
  const char data[] = " world";
  auto r = s.try_append(Span<const char>(data, 6));
  EXPECT_TRUE(r.is_ok());
  EXPECT_STREQ(s.c_str(), "hello world");
}

TEST(StringTest, TryAppendInvalid) {
  String s("hello");
  const char data[] = "\xC0\x80";
  auto r = s.try_append(Span<const char>(data, 2));
  EXPECT_TRUE(r.is_err());
  /* s is unchanged */
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, TryAppendEmpty) {
  String s("hello");
  auto r = s.try_append(Span<const char>());
  EXPECT_TRUE(r.is_ok());
  EXPECT_STREQ(s.c_str(), "hello");
}

/* ── Truncate ──────────────────────────────────────────────────────── */

TEST(StringTest, TruncateValid) {
  String s("hello");
  s.truncate(3);
  EXPECT_STREQ(s.c_str(), "hel");
  EXPECT_EQ(s.len(), 3u);
}

TEST(StringTest, TruncateToZero) {
  String s("hello");
  s.truncate(0);
  EXPECT_TRUE(s.is_empty());
}

TEST(StringTest, TruncateToSameLength) {
  String s("hello");
  s.truncate(5);
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, TruncateOnCharBoundary) {
  /* "中文" = E4 B8 AD E6 96 87, truncate after first char (3 bytes) */
  String s("\xE4\xB8\xAD\xE6\x96\x87");
  s.truncate(3);
  EXPECT_EQ(s.len(), 3u);
}

/* ── Clear / reserve / shrink_to_fit ───────────────────────────────── */

TEST(StringTest, Clear) {
  String s("hello");
  s.clear();
  EXPECT_TRUE(s.is_empty());
  EXPECT_STREQ(s.c_str(), "");
}

TEST(StringTest, Reserve) {
  String s("hi");
  s.reserve(100);
  EXPECT_GE(s.capacity(), 102u);
  EXPECT_STREQ(s.c_str(), "hi");
}

TEST(StringTest, ShrinkToFit) {
  String s("hi");
  s.reserve(1000);
  s.shrink_to_fit();
  /* capacity should be closer to len now */
  EXPECT_LE(s.capacity(), s.len() + 16);
  EXPECT_STREQ(s.c_str(), "hi");
}

/* ── Find ──────────────────────────────────────────────────────────── */

TEST(StringTest, FindCStrFound) {
  String s("hello world");
  auto pos = s.find("world");
  EXPECT_TRUE(pos.is_some());
  EXPECT_EQ(pos.unwrap(), 6u);
}

TEST(StringTest, FindCStrNotFound) {
  String s("hello");
  auto pos = s.find("xyz");
  EXPECT_TRUE(pos.is_none());
}

TEST(StringTest, FindSpanFound) {
  String s("hello world");
  const char needle[] = "llo";
  auto pos = s.find(Span<const char>(needle, 3));
  EXPECT_TRUE(pos.is_some());
  EXPECT_EQ(pos.unwrap(), 2u);
}

TEST(StringTest, FindSpanNotFound) {
  String s("hello");
  const char needle[] = "xyz";
  auto pos = s.find(Span<const char>(needle, 3));
  EXPECT_TRUE(pos.is_none());
}

/* ── Comparison ────────────────────────────────────────────────────── */

TEST(StringTest, EqualStrings) {
  String a("hello");
  String b("hello");
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(StringTest, NotEqualStrings) {
  String a("hello");
  String b("world");
  EXPECT_TRUE(a != b);
  EXPECT_FALSE(a == b);
}

TEST(StringTest, EqualWithCStr) {
  String s("hello");
  EXPECT_TRUE(s == "hello");
  EXPECT_TRUE("hello" == s);
  EXPECT_FALSE(s == "world");
  EXPECT_FALSE("world" == s);
}

TEST(StringTest, NotEqualWithCStr) {
  String s("hello");
  EXPECT_TRUE(s != "world");
  EXPECT_TRUE("world" != s);
  EXPECT_FALSE(s != "hello");
}

/* ── operator+= ───────────────────────────────────────────────────── */

TEST(StringTest, PlusEqualCStr) {
  String s("hello");
  s += " world";
  EXPECT_STREQ(s.c_str(), "hello world");
}

TEST(StringTest, PlusEqualString) {
  String a("hello");
  String b(" world");
  a += b;
  EXPECT_STREQ(a.c_str(), "hello world");
}

/* ── C interop ─────────────────────────────────────────────────────── */

TEST(StringTest, ReleaseAndFromRaw) {
  String s("hello");
  xString raw = s.release();
  EXPECT_TRUE(s.is_empty());  /* s is now moved-from */

  String s2 = String::from_raw(raw);
  EXPECT_STREQ(s2.c_str(), "hello");
}

/* ── UTF-8 content in operations ───────────────────────────────────── */

TEST(StringTest, AppendMultibyte) {
  String s("hello ");
  s.append("\xE4\xB8\x96\xE7\x95\x8C");  /* "世界" */
  EXPECT_EQ(s.len(), 12u);
}

TEST(StringTest, FindMultibyte) {
  /* "hello世界" */
  String s("hello\xE4\xB8\x96\xE7\x95\x8C");
  auto pos = s.find("\xE7\x95\x8C");  /* "界" */
  EXPECT_TRUE(pos.is_some());
  EXPECT_EQ(pos.unwrap(), 8u);
}

TEST(StringTest, CloneMultibyte) {
  String s("\xF0\x9F\x98\x80 smile");
  String c = s.clone();
  EXPECT_STREQ(c.c_str(), s.c_str());
  EXPECT_EQ(c.len(), s.len());
}

/* ── Death tests (debug only) ──────────────────────────────────────── */

#ifndef NDEBUG

TEST(StringDeathTest, ConstructFromInvalid) {
  EXPECT_DEATH(String("\xC0\x80"), "");
}

TEST(StringDeathTest, AppendInvalid) {
  String s("hello");
  EXPECT_DEATH(s.append("\xFE\xFF"), "");
}

TEST(StringDeathTest, TruncateNotOnBoundary) {
  /* "中文" = E4 B8 AD E6 96 87, byte 1 is a continuation byte */
  String s("\xE4\xB8\xAD\xE6\x96\x87");
  EXPECT_DEATH(s.truncate(1), "");
}

TEST(StringDeathTest, TruncateExceedsLen) {
  String s("hi");
  EXPECT_DEATH(s.truncate(10), "");
}

#endif // NDEBUG
