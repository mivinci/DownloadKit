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

using xpp::Span;
using xpp::String;

/* ── Construction ──────────────────────────────────────────────────── */

TEST(StringTest, DefaultConstruction) {
  String s;
  EXPECT_EQ(s.len(), 0u);
  EXPECT_EQ(s.capacity(), 0u);
  EXPECT_TRUE(s.is_empty());
  EXPECT_TRUE(s.empty());
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
  EXPECT_TRUE(s.is_empty());
}

TEST(StringTest, FromUtf8Multibyte) {
  String s("\xE4\xB8\xAD\xE6\x96\x87");
  EXPECT_EQ(s.len(), 6u);
  EXPECT_STREQ(s.c_str(), "\xE4\xB8\xAD\xE6\x96\x87");
}

TEST(StringTest, FromEmoji) {
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
  const char data[] = "\xC0\x80";
  auto r = String::from(Span<const char>(data, 2));
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().valid_up_to, 0u);
}

TEST(StringTest, FromInvalidBytesMiddle) {
  const char data[] = "abc\xFE xyz";
  auto r = String::from(Span<const char>(data, 8));
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().valid_up_to, 3u);
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

/* ── Move semantics ────────────────────────────────────────────────── */

TEST(StringTest, MoveConstruction) {
  String a("hello");
  String b(std::move(a));
  EXPECT_STREQ(b.c_str(), "hello");
  EXPECT_TRUE(a.is_empty());
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
  EXPECT_EQ(a.len(), 0u);
  EXPECT_STREQ(a.c_str(), "");
}

/* ── Clone ─────────────────────────────────────────────────────────── */

TEST(StringTest, Clone) {
  String a("hello");
  String b = a.clone();
  EXPECT_STREQ(b.c_str(), "hello");
  EXPECT_STREQ(a.c_str(), "hello");
}

/* ── as_span ───────────────────────────────────────────────────────── */

TEST(StringTest, AsSpan) {
  String s("hello");
  Span<const char> sp = s.as_span();
  EXPECT_EQ(sp.size(), 5u);
  EXPECT_EQ(sp[0], 'h');
}

TEST(StringTest, AsSpanEmpty) {
  String s;
  EXPECT_EQ(s.as_span().size(), 0u);
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

TEST(StringTest, AppendToEmpty) {
  String s;
  s.append("hello");
  EXPECT_STREQ(s.c_str(), "hello");
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
  const char data[] = "ok\xC0\x80";
  auto r = s.try_append(Span<const char>(data, 4));
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().valid_up_to, 2u);
  EXPECT_STREQ(s.c_str(), "hello");
}

/* ── Truncate ──────────────────────────────────────────────────────── */

TEST(StringTest, TruncateValid) {
  String s("hello");
  s.truncate(3);
  EXPECT_STREQ(s.c_str(), "hel");
}

TEST(StringTest, TruncateToZero) {
  String s("hello");
  s.truncate(0);
  EXPECT_TRUE(s.is_empty());
}

TEST(StringTest, TruncateOnCharBoundary) {
  String s("\xE4\xB8\xAD\xE6\x96\x87");
  s.truncate(3);
  EXPECT_EQ(s.len(), 3u);
  EXPECT_STREQ(s.c_str(), "\xE4\xB8\xAD");
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

/* ── Find ──────────────────────────────────────────────────────────── */

TEST(StringTest, FindCStrFound) {
  String s("hello world");
  auto pos = s.find("world");
  EXPECT_TRUE(pos.is_some());
  EXPECT_EQ(pos.unwrap(), 6u);
}

TEST(StringTest, FindCStrNotFound) {
  String s("hello");
  EXPECT_TRUE(s.find("xyz").is_none());
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
  EXPECT_TRUE(s.find(Span<const char>(needle, 3)).is_none());
}

TEST(StringTest, FindOnEmpty) {
  String s;
  EXPECT_TRUE(s.find("x").is_none());
  EXPECT_TRUE(s.find(Span<const char>("x", 1)).is_none());
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
}

TEST(StringTest, EqualWithCStr) {
  String s("hello");
  EXPECT_TRUE(s == "hello");
  EXPECT_TRUE("hello" == s);
}

TEST(StringTest, NotEqualWithCStr) {
  String s("hello");
  EXPECT_TRUE(s != "world");
  EXPECT_TRUE("world" != s);
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

/* ── into_vec / as_vec ─────────────────────────────────────────────── */

TEST(StringTest, AsVec) {
  String s("hello");
  const auto &v = s.as_vec();
  EXPECT_EQ(v.len(), 5u);
  EXPECT_EQ(v[0], 'h');
}

TEST(StringTest, IntoVec) {
  String s("hello");
  auto v = s.into_vec();
  EXPECT_EQ(v.len(), 5u);
  EXPECT_TRUE(s.is_empty());
}

/* ── UTF-8 in operations ───────────────────────────────────────────── */

TEST(StringTest, AppendMultibyte) {
  String s("hello ");
  s.append("\xE4\xB8\x96\xE7\x95\x8C");
  EXPECT_EQ(s.len(), 12u);
}

TEST(StringTest, FindMultibyte) {
  String s("hello\xE4\xB8\x96\xE7\x95\x8C");
  auto pos = s.find("\xE7\x95\x8C");
  EXPECT_TRUE(pos.is_some());
  EXPECT_EQ(pos.unwrap(), 8u);
}

TEST(StringTest, CloneMultibyte) {
  String s("\xF0\x9F\x98\x80 smile");
  String c = s.clone();
  EXPECT_STREQ(c.c_str(), s.c_str());
}

/* ── Additional edge cases ─────────────────────────────────────────── */

TEST(StringTest, Len) {
  String s("hello");
  EXPECT_EQ(s.len(), 5u);
}

TEST(StringTest, Capacity) {
  String s("hello");
  EXPECT_GE(s.capacity(), 5u);
}

TEST(StringTest, IsEmptyAndEmpty) {
  String empty;
  String nonempty("x");
  EXPECT_TRUE(empty.is_empty());
  EXPECT_TRUE(empty.empty());
  EXPECT_FALSE(nonempty.is_empty());
  EXPECT_FALSE(nonempty.empty());
}

TEST(StringTest, ShrinkToFit) {
  String s("hi");
  s.reserve(1000);
  EXPECT_GE(s.capacity(), 1000u);
  s.shrink_to_fit();
  EXPECT_STREQ(s.c_str(), "hi");
}

TEST(StringTest, AppendEmpty) {
  String s("hello");
  s.append("");
  s.append(nullptr);
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, TryAppendEmpty) {
  String s("hello");
  auto r = s.try_append(Span<const char>());
  EXPECT_TRUE(r.is_ok());
  EXPECT_STREQ(s.c_str(), "hello");
}

TEST(StringTest, FromUncheckedNullptr) {
  auto s = String::from_unchecked(nullptr);
  EXPECT_TRUE(s.is_empty());
}

TEST(StringTest, FromUncheckedZeroLen) {
  auto s = String::from_unchecked("hello", 0);
  EXPECT_TRUE(s.is_empty());
}

TEST(StringTest, CloneEmpty) {
  String s;
  String c = s.clone();
  EXPECT_TRUE(c.is_empty());
  EXPECT_STREQ(c.c_str(), "");
}

TEST(StringTest, EqualBothEmpty) {
  String a;
  String b;
  EXPECT_TRUE(a == b);
  EXPECT_TRUE(a == "");
  EXPECT_TRUE("" == a);
}

/* ── Death tests (debug only) ──────────────────────────────────────── */

#if XPP_DEBUG

TEST(StringDeathTest, ConstructFromInvalid) {
  EXPECT_DEATH(String("\xC0\x80"), "");
}

TEST(StringDeathTest, AppendInvalid) {
  String s("hello");
  EXPECT_DEATH(s.append("\xFE\xFF"), "");
}

TEST(StringDeathTest, TruncateNotOnBoundary) {
  String s("\xE4\xB8\xAD\xE6\x96\x87");
  EXPECT_DEATH(s.truncate(1), "");
}

TEST(StringDeathTest, TruncateExceedsLen) {
  String s("hi");
  EXPECT_DEATH(s.truncate(10), "");
}

#endif // XPP_DEBUG
