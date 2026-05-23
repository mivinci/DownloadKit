/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * span_test.cpp - Unit tests for xpp::Span<T>
 */

#include <xpp/span.h>

#include <gtest/gtest.h>

#include <array>
#include <vector>

using xpp::Span;

/* ── Construction ──────────────────────────────────────────────────── */

TEST(SpanTest, DefaultConstruction) {
  Span<int> s;
  EXPECT_EQ(s.data(), nullptr);
  EXPECT_EQ(s.size(), 0u);
  EXPECT_TRUE(s.is_empty());
}

TEST(SpanTest, PointerAndLength) {
  int arr[] = {10, 20, 30};
  Span<int> s(arr, 3);
  EXPECT_EQ(s.data(), arr);
  EXPECT_EQ(s.size(), 3u);
  EXPECT_FALSE(s.is_empty());
}

TEST(SpanTest, CArray) {
  int arr[] = {1, 2, 3, 4, 5};
  Span<int> s(arr);
  EXPECT_EQ(s.data(), arr);
  EXPECT_EQ(s.size(), 5u);
}

TEST(SpanTest, FromVector) {
  std::vector<int> v = {7, 8, 9};
  Span<int> s(v.data(), v.size());
  EXPECT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0], 7);
}

TEST(SpanTest, CopyConstruction) {
  int arr[] = {1, 2, 3};
  Span<int> a(arr, 3);
  Span<int> b(a);
  EXPECT_EQ(b.data(), arr);
  EXPECT_EQ(b.size(), 3u);
}

TEST(SpanTest, CopyAssignment) {
  int arr[] = {1, 2, 3};
  Span<int> a(arr, 3);
  Span<int> b;
  b = a;
  EXPECT_EQ(b.data(), arr);
  EXPECT_EQ(b.size(), 3u);
}

/* ── Accessors ─────────────────────────────────────────────────────── */

TEST(SpanTest, SizeBytes) {
  double arr[] = {1.0, 2.0, 3.0};
  Span<double> s(arr);
  EXPECT_EQ(s.size_bytes(), 3 * sizeof(double));
}

TEST(SpanTest, IsEmpty) {
  Span<int> empty;
  EXPECT_TRUE(empty.is_empty());

  int x = 42;
  Span<int> non_empty(&x, 1);
  EXPECT_FALSE(non_empty.is_empty());
}

/* ── Element access ────────────────────────────────────────────────── */

TEST(SpanTest, Subscript) {
  int arr[] = {10, 20, 30, 40};
  Span<int> s(arr);
  EXPECT_EQ(s[0], 10);
  EXPECT_EQ(s[1], 20);
  EXPECT_EQ(s[2], 30);
  EXPECT_EQ(s[3], 40);
}

TEST(SpanTest, SubscriptMutation) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  s[1] = 99;
  EXPECT_EQ(arr[1], 99);
}

TEST(SpanTest, FrontAndBack) {
  int arr[] = {5, 6, 7, 8};
  Span<int> s(arr);
  EXPECT_EQ(s.front(), 5);
  EXPECT_EQ(s.back(), 8);
}

/* ── Subspans ──────────────────────────────────────────────────────── */

TEST(SpanTest, First) {
  int arr[] = {1, 2, 3, 4, 5};
  Span<int> s(arr);
  auto f = s.first(3);
  EXPECT_EQ(f.size(), 3u);
  EXPECT_EQ(f[0], 1);
  EXPECT_EQ(f[2], 3);
}

TEST(SpanTest, FirstZero) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  auto f = s.first(0);
  EXPECT_TRUE(f.is_empty());
}

TEST(SpanTest, Last) {
  int arr[] = {1, 2, 3, 4, 5};
  Span<int> s(arr);
  auto l = s.last(2);
  EXPECT_EQ(l.size(), 2u);
  EXPECT_EQ(l[0], 4);
  EXPECT_EQ(l[1], 5);
}

TEST(SpanTest, SubspanOffsetOnly) {
  int arr[] = {10, 20, 30, 40, 50};
  Span<int> s(arr);
  auto sub = s.subspan(2);
  EXPECT_EQ(sub.size(), 3u);
  EXPECT_EQ(sub[0], 30);
  EXPECT_EQ(sub[2], 50);
}

TEST(SpanTest, SubspanOffsetAndCount) {
  int arr[] = {10, 20, 30, 40, 50};
  Span<int> s(arr);
  auto sub = s.subspan(1, 2);
  EXPECT_EQ(sub.size(), 2u);
  EXPECT_EQ(sub[0], 20);
  EXPECT_EQ(sub[1], 30);
}

TEST(SpanTest, SubspanFull) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  auto sub = s.subspan(0);
  EXPECT_EQ(sub.size(), 3u);
  EXPECT_EQ(sub.data(), arr);
}

TEST(SpanTest, SubspanEmpty) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  auto sub = s.subspan(3);
  EXPECT_TRUE(sub.is_empty());
}

/* ── Iterators ─────────────────────────────────────────────────────── */

TEST(SpanTest, BeginEnd) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  EXPECT_EQ(s.begin(), &arr[0]);
  EXPECT_EQ(s.end(), &arr[3]);
}

TEST(SpanTest, RangeFor) {
  int arr[] = {1, 2, 3, 4};
  Span<int> s(arr);
  int sum = 0;
  for (int &v : s) sum += v;
  EXPECT_EQ(sum, 10);
}

TEST(SpanTest, RangeForMutation) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  for (int &v : s) v *= 2;
  EXPECT_EQ(arr[0], 2);
  EXPECT_EQ(arr[1], 4);
  EXPECT_EQ(arr[2], 6);
}

TEST(SpanTest, EmptyBeginEnd) {
  Span<int> s;
  EXPECT_EQ(s.begin(), s.end());
}

/* ── Conversion ────────────────────────────────────────────────────── */

TEST(SpanTest, MutableToConst) {
  int arr[] = {1, 2, 3};
  Span<int> mutable_span(arr);
  Span<const int> const_span = mutable_span;
  EXPECT_EQ(const_span.data(), arr);
  EXPECT_EQ(const_span.size(), 3u);
}

TEST(SpanTest, ConstSpanPreventsMutation) {
  int arr[] = {1, 2, 3};
  Span<const int> s(arr, 3);
  // s[0] = 99; // This should not compile
  EXPECT_EQ(s[0], 1);
}

/* ── Comparison ────────────────────────────────────────────────────── */

TEST(SpanTest, EqualSameData) {
  int arr[] = {1, 2, 3};
  Span<int> a(arr);
  Span<int> b(arr);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(SpanTest, EqualDifferentData) {
  int arr1[] = {1, 2, 3};
  int arr2[] = {1, 2, 3};
  Span<int> a(arr1);
  Span<int> b(arr2);
  EXPECT_TRUE(a == b);
}

TEST(SpanTest, NotEqualDifferentSize) {
  int arr[] = {1, 2, 3};
  Span<int> a(arr, 2);
  Span<int> b(arr, 3);
  EXPECT_TRUE(a != b);
}

TEST(SpanTest, NotEqualDifferentValues) {
  int arr1[] = {1, 2, 3};
  int arr2[] = {1, 2, 4};
  Span<int> a(arr1);
  Span<int> b(arr2);
  EXPECT_TRUE(a != b);
}

TEST(SpanTest, EmptySpansEqual) {
  Span<int> a;
  Span<int> b;
  EXPECT_TRUE(a == b);
}

/* ── Size guarantees ───────────────────────────────────────────────── */

TEST(SpanTest, SizeofGuarantee) {
  static_assert(sizeof(Span<int>) == sizeof(int *) + sizeof(size_t),
                "Span<T> must be pointer + size");
  static_assert(sizeof(Span<char>) == sizeof(char *) + sizeof(size_t),
                "Span<T> must be pointer + size");
}

/* ── Death tests (debug only) ──────────────────────────────────────── */

#if XPP_DEBUG

using SpanDeathTest = ::testing::Test;

TEST(SpanDeathTest, SubscriptOutOfBounds) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  EXPECT_DEATH(s[3], "");
}

TEST(SpanDeathTest, FrontOnEmpty) {
  Span<int> s;
  EXPECT_DEATH(s.front(), "");
}

TEST(SpanDeathTest, BackOnEmpty) {
  Span<int> s;
  EXPECT_DEATH(s.back(), "");
}

TEST(SpanDeathTest, FirstExceedsSize) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  EXPECT_DEATH(s.first(4), "");
}

TEST(SpanDeathTest, LastExceedsSize) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  EXPECT_DEATH(s.last(4), "");
}

TEST(SpanDeathTest, SubspanOffsetExceedsSize) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  EXPECT_DEATH(s.subspan(4), "");
}

TEST(SpanDeathTest, SubspanCountExceedsRemaining) {
  int arr[] = {1, 2, 3};
  Span<int> s(arr);
  EXPECT_DEATH(s.subspan(1, 5), "");
}

#endif // XPP_DEBUG
