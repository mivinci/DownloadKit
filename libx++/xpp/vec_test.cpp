/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * vec_test.cpp - Unit tests for xpp::Vec<T>
 */

#include <xpp/vec.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

using xpp::Span;
using xpp::Vec;

/* ── Helper: non-trivial type for lifecycle tracking ─────────────── */

static int g_ctor_count = 0;
static int g_dtor_count = 0;

struct Tracked {
  int value;
  Tracked() : value(0) { ++g_ctor_count; }
  explicit Tracked(int v) : value(v) { ++g_ctor_count; }
  Tracked(const Tracked &o) : value(o.value) { ++g_ctor_count; }
  Tracked(Tracked &&o) noexcept : value(o.value) {
    o.value = -1;
    ++g_ctor_count;
  }
  ~Tracked() { ++g_dtor_count; }
  bool operator==(const Tracked &o) const { return value == o.value; }
};

class VecTrackedTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_ctor_count = 0;
    g_dtor_count = 0;
  }
};

/* ── Construction ──────────────────────────────────────────────────── */

TEST(VecTest, DefaultConstruction) {
  Vec<int> v;
  EXPECT_EQ(v.len(), 0u);
  EXPECT_EQ(v.capacity(), 0u);
  EXPECT_TRUE(v.is_empty());
  EXPECT_EQ(v.data(), nullptr);
}

TEST(VecTest, ConstructWithCount) {
  Vec<int> v(5, 42);
  EXPECT_EQ(v.len(), 5u);
  EXPECT_GE(v.capacity(), 5u);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(v[i], 42);
  }
}

TEST(VecTest, ConstructWithCountDefault) {
  Vec<int> v(3);
  EXPECT_EQ(v.len(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(v[i], 0);
  }
}

/* ── Move semantics ────────────────────────────────────────────────── */

TEST(VecTest, MoveConstruction) {
  Vec<int> a(3, 7);
  Vec<int> b(std::move(a));
  EXPECT_EQ(b.len(), 3u);
  EXPECT_EQ(b[0], 7);
  EXPECT_TRUE(a.is_empty());
  EXPECT_EQ(a.data(), nullptr);
}

TEST(VecTest, MoveAssignment) {
  Vec<int> a(3, 7);
  Vec<int> b(2, 99);
  b = std::move(a);
  EXPECT_EQ(b.len(), 3u);
  EXPECT_EQ(b[0], 7);
  EXPECT_TRUE(a.is_empty());
}

/* ── Clone ─────────────────────────────────────────────────────────── */

TEST(VecTest, Clone) {
  Vec<int> a(3, 42);
  Vec<int> b = a.clone();
  EXPECT_EQ(b.len(), 3u);
  EXPECT_EQ(b[0], 42);
  /* Independent */
  b[0] = 99;
  EXPECT_EQ(a[0], 42);
}

/* ── Push / Pop ────────────────────────────────────────────────────── */

TEST(VecTest, PushAndAccess) {
  Vec<int> v;
  v.push(10);
  v.push(20);
  v.push(30);
  EXPECT_EQ(v.len(), 3u);
  EXPECT_EQ(v[0], 10);
  EXPECT_EQ(v[1], 20);
  EXPECT_EQ(v[2], 30);
}

TEST(VecTest, PushMove) {
  Vec<std::string> v;
  std::string s = "hello";
  v.push(std::move(s));
  EXPECT_EQ(v[0], "hello");
  EXPECT_TRUE(s.empty()); /* moved-from */
}

TEST(VecTest, Pop) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  auto val = v.pop();
  EXPECT_TRUE(val.is_some());
  EXPECT_EQ(val.unwrap(), 3);
  EXPECT_EQ(v.len(), 2u);
}

TEST(VecTest, PopEmpty) {
  Vec<int> v;
  auto val = v.pop();
  EXPECT_TRUE(val.is_none());
}

/* ── Extend ────────────────────────────────────────────────────────── */

TEST(VecTest, Extend) {
  Vec<int> v;
  v.push(1);
  int arr[] = {2, 3, 4};
  v.extend(Span<const int>(arr, 3));
  EXPECT_EQ(v.len(), 4u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[3], 4);
}

TEST(VecTest, ExtendEmpty) {
  Vec<int> v;
  v.push(1);
  v.extend(Span<const int>());
  EXPECT_EQ(v.len(), 1u);
}

/* ── Front / Back ──────────────────────────────────────────────────── */

TEST(VecTest, FrontAndBack) {
  Vec<int> v;
  v.push(10);
  v.push(20);
  v.push(30);
  EXPECT_EQ(v.front(), 10);
  EXPECT_EQ(v.back(), 30);
}

/* ── Clear / Truncate ──────────────────────────────────────────────── */

TEST(VecTest, Clear) {
  Vec<int> v(5, 1);
  v.clear();
  EXPECT_TRUE(v.is_empty());
  EXPECT_GE(v.capacity(), 5u); /* allocation not freed */
}

TEST(VecTest, Truncate) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  v.truncate(1);
  EXPECT_EQ(v.len(), 1u);
  EXPECT_EQ(v[0], 1);
}

TEST(VecTest, TruncateToZero) {
  Vec<int> v(3, 42);
  v.truncate(0);
  EXPECT_TRUE(v.is_empty());
}

/* ── Reserve / ShrinkToFit ─────────────────────────────────────────── */

TEST(VecTest, Reserve) {
  Vec<int> v;
  v.reserve(100);
  EXPECT_GE(v.capacity(), 100u);
  EXPECT_TRUE(v.is_empty());
}

TEST(VecTest, ShrinkToFit) {
  Vec<int> v;
  v.reserve(1000);
  v.push(1);
  v.push(2);
  v.shrink_to_fit();
  EXPECT_EQ(v.capacity(), 2u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);
}

TEST(VecTest, ShrinkToFitEmpty) {
  Vec<int> v;
  v.reserve(100);
  v.shrink_to_fit();
  EXPECT_EQ(v.capacity(), 0u);
  EXPECT_EQ(v.data(), nullptr);
}

/* ── as_span ───────────────────────────────────────────────────────── */

TEST(VecTest, AsSpan) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  Span<const int> sp = v.as_span();
  EXPECT_EQ(sp.size(), 3u);
  EXPECT_EQ(sp[0], 1);
  EXPECT_EQ(sp[2], 3);
}

TEST(VecTest, AsSpanMutable) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  Span<int> sp = v.as_span();
  sp[0] = 99;
  EXPECT_EQ(v[0], 99);
}

/* ── Iterators ─────────────────────────────────────────────────────── */

TEST(VecTest, RangeFor) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  int sum = 0;
  for (int &x : v) sum += x;
  EXPECT_EQ(sum, 6);
}

TEST(VecTest, ConstRangeFor) {
  Vec<int> v;
  v.push(10);
  v.push(20);
  const Vec<int> &cv = v;
  int sum = 0;
  for (const int &x : cv) sum += x;
  EXPECT_EQ(sum, 30);
}

/* ── with_capacity ─────────────────────────────────────────────────── */

TEST(VecTest, WithCapacity) {
  auto v = Vec<int>::with_capacity(100);
  EXPECT_EQ(v.len(), 0u);
  EXPECT_GE(v.capacity(), 100u);
  EXPECT_TRUE(v.is_empty());
}

/* ── Insert ────────────────────────────────────────────────────────── */

TEST(VecTest, InsertAtBeginning) {
  Vec<int> v;
  v.push(2);
  v.push(3);
  v.insert(0, 1);
  EXPECT_EQ(v.len(), 3u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);
  EXPECT_EQ(v[2], 3);
}

TEST(VecTest, InsertInMiddle) {
  Vec<int> v;
  v.push(1);
  v.push(3);
  v.insert(1, 2);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);
  EXPECT_EQ(v[2], 3);
}

TEST(VecTest, InsertAtEnd) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.insert(2, 3);
  EXPECT_EQ(v[2], 3);
  EXPECT_EQ(v.len(), 3u);
}

TEST(VecTest, InsertMove) {
  Vec<std::string> v;
  v.push("a");
  v.push("c");
  std::string b = "b";
  v.insert(1, std::move(b));
  EXPECT_EQ(v[1], "b");
  EXPECT_TRUE(b.empty());
}

/* ── Remove ────────────────────────────────────────────────────────── */

TEST(VecTest, RemoveFromBeginning) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  int val = v.remove(0);
  EXPECT_EQ(val, 1);
  EXPECT_EQ(v.len(), 2u);
  EXPECT_EQ(v[0], 2);
  EXPECT_EQ(v[1], 3);
}

TEST(VecTest, RemoveFromMiddle) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  int val = v.remove(1);
  EXPECT_EQ(val, 2);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 3);
}

TEST(VecTest, RemoveFromEnd) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  int val = v.remove(2);
  EXPECT_EQ(val, 3);
  EXPECT_EQ(v.len(), 2u);
}

/* ── SwapRemove ────────────────────────────────────────────────────── */

TEST(VecTest, SwapRemove) {
  Vec<int> v;
  v.push(10);
  v.push(20);
  v.push(30);
  v.push(40);
  int val = v.swap_remove(1);
  EXPECT_EQ(val, 20);
  EXPECT_EQ(v.len(), 3u);
  EXPECT_EQ(v[0], 10);
  EXPECT_EQ(v[1], 40);  /* last moved into slot 1 */
  EXPECT_EQ(v[2], 30);
}

TEST(VecTest, SwapRemoveLast) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  int val = v.swap_remove(1);
  EXPECT_EQ(val, 2);
  EXPECT_EQ(v.len(), 1u);
  EXPECT_EQ(v[0], 1);
}

/* ── Append ────────────────────────────────────────────────────────── */

TEST(VecTest, AppendDrains) {
  Vec<int> a;
  a.push(1);
  a.push(2);
  Vec<int> b;
  b.push(3);
  b.push(4);
  a.append(b);
  EXPECT_EQ(a.len(), 4u);
  EXPECT_EQ(a[0], 1);
  EXPECT_EQ(a[3], 4);
  EXPECT_TRUE(b.is_empty());  /* drained */
}

TEST(VecTest, AppendEmpty) {
  Vec<int> a;
  a.push(1);
  Vec<int> b;
  a.append(b);
  EXPECT_EQ(a.len(), 1u);
}

/* ── Resize ────────────────────────────────────────────────────────── */

TEST(VecTest, ResizeGrow) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.resize(5, 99);
  EXPECT_EQ(v.len(), 5u);
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], 2);
  EXPECT_EQ(v[2], 99);
  EXPECT_EQ(v[4], 99);
}

TEST(VecTest, ResizeShrink) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.push(3);
  v.resize(1);
  EXPECT_EQ(v.len(), 1u);
  EXPECT_EQ(v[0], 1);
}

TEST(VecTest, ResizeSameLen) {
  Vec<int> v;
  v.push(42);
  v.resize(1, 0);
  EXPECT_EQ(v.len(), 1u);
  EXPECT_EQ(v[0], 42);
}

/* ── Retain ────────────────────────────────────────────────────────── */

TEST(VecTest, RetainEven) {
  Vec<int> v;
  for (int i = 1; i <= 6; ++i) v.push(i);
  v.retain([](const int &x) { return x % 2 == 0; });
  EXPECT_EQ(v.len(), 3u);
  EXPECT_EQ(v[0], 2);
  EXPECT_EQ(v[1], 4);
  EXPECT_EQ(v[2], 6);
}

TEST(VecTest, RetainAll) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.retain([](const int &) { return true; });
  EXPECT_EQ(v.len(), 2u);
}

TEST(VecTest, RetainNone) {
  Vec<int> v;
  v.push(1);
  v.push(2);
  v.retain([](const int &) { return false; });
  EXPECT_TRUE(v.is_empty());
}

/* ── Comparison ────────────────────────────────────────────────────── */

TEST(VecTest, Equal) {
  Vec<int> a;
  a.push(1);
  a.push(2);
  Vec<int> b;
  b.push(1);
  b.push(2);
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(VecTest, NotEqualDifferentLen) {
  Vec<int> a;
  a.push(1);
  Vec<int> b;
  b.push(1);
  b.push(2);
  EXPECT_TRUE(a != b);
}

TEST(VecTest, NotEqualDifferentValues) {
  Vec<int> a;
  a.push(1);
  a.push(2);
  Vec<int> b;
  b.push(1);
  b.push(3);
  EXPECT_TRUE(a != b);
}

/* ── Growth ────────────────────────────────────────────────────────── */

TEST(VecTest, GrowthDoublesCapacity) {
  Vec<int> v;
  v.push(0);  /* initial cap = 4 */
  size_t cap_after_first = v.capacity();
  EXPECT_GE(cap_after_first, 4u);
  /* Fill to capacity */
  while (v.len() < cap_after_first) v.push(0);
  /* Next push should double */
  v.push(0);
  EXPECT_GE(v.capacity(), cap_after_first * 2);
}

/* ── Non-trivial type lifecycle ────────────────────────────────────── */

TEST_F(VecTrackedTest, DestroyOnDrop) {
  {
    Vec<Tracked> v;
    v.push(Tracked(1));
    v.push(Tracked(2));
    v.push(Tracked(3));
  }
  /* All 3 elements should have been destroyed (plus temporaries) */
  EXPECT_EQ(g_ctor_count, g_dtor_count);
}

TEST_F(VecTrackedTest, DestroyOnClear) {
  Vec<Tracked> v;
  v.push(Tracked(1));
  v.push(Tracked(2));
  int dtors_before = g_dtor_count;
  v.clear();
  /* At least 2 destructor calls (the stored elements) */
  EXPECT_GE(g_dtor_count - dtors_before, 2);
  EXPECT_TRUE(v.is_empty());
}

TEST_F(VecTrackedTest, DestroyOnTruncate) {
  Vec<Tracked> v;
  v.push(Tracked(1));
  v.push(Tracked(2));
  v.push(Tracked(3));
  int dtors_before = g_dtor_count;
  v.truncate(1);
  /* 2 elements destroyed */
  EXPECT_GE(g_dtor_count - dtors_before, 2);
  EXPECT_EQ(v.len(), 1u);
  EXPECT_EQ(v[0].value, 1);
}

TEST_F(VecTrackedTest, PopDestroysElement) {
  Vec<Tracked> v;
  v.push(Tracked(42));
  int dtors_before = g_dtor_count;
  auto val = v.pop();
  EXPECT_TRUE(val.is_some());
  EXPECT_EQ(val.unwrap().value, 42);
  /* The in-place element was destroyed after move */
  EXPECT_GT(g_dtor_count, dtors_before);
}

/* ── sizeof guarantee ──────────────────────────────────────────────── */

TEST(VecTest, SizeofGuarantee) {
  static_assert(sizeof(Vec<int>) == sizeof(int *) + 2 * sizeof(size_t),
                "Vec<T> must be ptr + len + cap");
  static_assert(sizeof(Vec<char>) == sizeof(char *) + 2 * sizeof(size_t),
                "Vec<T> must be ptr + len + cap");
}

/* ── Death tests (debug only) ──────────────────────────────────────── */

#ifndef NDEBUG

TEST(VecDeathTest, SubscriptOutOfBounds) {
  Vec<int> v;
  v.push(1);
  EXPECT_DEATH(v[1], "");
}

TEST(VecDeathTest, FrontOnEmpty) {
  Vec<int> v;
  EXPECT_DEATH(v.front(), "");
}

TEST(VecDeathTest, BackOnEmpty) {
  Vec<int> v;
  EXPECT_DEATH(v.back(), "");
}

TEST(VecDeathTest, TruncateExceedsLen) {
  Vec<int> v;
  v.push(1);
  EXPECT_DEATH(v.truncate(5), "");
}

TEST(VecDeathTest, InsertExceedsLen) {
  Vec<int> v;
  v.push(1);
  EXPECT_DEATH(v.insert(5, 99), "");
}

TEST(VecDeathTest, RemoveOutOfBounds) {
  Vec<int> v;
  v.push(1);
  EXPECT_DEATH(v.remove(1), "");
}

TEST(VecDeathTest, SwapRemoveOutOfBounds) {
  Vec<int> v;
  v.push(1);
  EXPECT_DEATH(v.swap_remove(1), "");
}

#endif // NDEBUG
