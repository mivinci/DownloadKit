/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nonnull_test.cpp - Tests for NonNull<T> and Option<NonNull<T>>.
 *
 * Covers ctors, accessors, niche-optimized Option storage, all
 * combinators, void-pointer SFINAE, and death-test contract checks.
 */

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

#include <xpp/nonnull.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::NonNull<int>) == sizeof(int *), "NonNull<int> must be sizeof(int*)");
static_assert(sizeof(xpp::Option<xpp::NonNull<int>>) == sizeof(int *),
              "Option<NonNull<int>> niche broken");
static_assert(std::is_trivially_copyable<xpp::NonNull<int>>::value,
              "NonNull<int> must be trivially copyable");
// Option<NonNull<int>> is NOT trivially copyable because its move ctor
// nulls the source (matching main Option<T>'s "moved-from is None"
// invariant). It is still trivially destructible — verified below.
static_assert(std::is_trivially_destructible<xpp::Option<xpp::NonNull<int>>>::value,
              "Option<NonNull<int>> must be trivially destructible");

namespace {

/* SFINAE detector for operator*. Used to verify NonNull<void> SFINAEs out. */
template <class, class = void> struct has_op_star : std::false_type {};
template <class T> struct has_op_star<T, decltype(void(*std::declval<T &>()))> : std::true_type {};

static_assert(has_op_star<xpp::NonNull<int>>::value, "NonNull<int> must have operator*");
static_assert(!has_op_star<xpp::NonNull<void>>::value, "NonNull<void> must not have operator*");

} // namespace

/* ── NonNull<T> construction ─────────────────────────────────────────── */

TEST(NonNullTest, ConstructFromReference) {
  int               x = 42;
  xpp::NonNull<int> p(x);
  EXPECT_EQ(p.get(), &x);
  EXPECT_EQ(*p, 42);
}

TEST(NonNullTest, NewUncheckedHappyPath) {
  int  x = 7;
  auto p = xpp::NonNull<int>::new_unchecked(&x);
  EXPECT_EQ(p.get(), &x);
  EXPECT_EQ(*p, 7);
}

#ifndef NDEBUG
TEST(NonNullDeathTest, NewUncheckedOnNullDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] { (void)xpp::NonNull<int>::new_unchecked(nullptr); }()),
               "NonNull::new_unchecked: pointer is null");
}
#endif

TEST(NonNullTest, FromNonNullReturnsSome) {
  int  x   = 99;
  auto opt = xpp::NonNull<int>::from(&x);
  EXPECT_TRUE(opt.is_some());
  EXPECT_EQ(opt.unwrap().get(), &x);
}

TEST(NonNullTest, FromNullptrReturnsNone) {
  int *p   = nullptr;
  auto opt = xpp::NonNull<int>::from(p);
  EXPECT_TRUE(opt.is_none());
}

TEST(NonNullTest, ArrowAccessor) {
  struct S {
    int v;
  };
  S               s{5};
  xpp::NonNull<S> p(s);
  EXPECT_EQ(p->v, 5);
}

TEST(NonNullTest, EqualityCompares) {
  int               x = 1, y = 2;
  xpp::NonNull<int> a(x);
  xpp::NonNull<int> b(x);
  xpp::NonNull<int> c(y);
  EXPECT_TRUE(a == b);
  EXPECT_TRUE(a != c);
}

TEST(NonNullTest, VoidVariantCompiles) {
  int   x   = 1;
  void *raw = &x;
  auto  opt = xpp::NonNull<void>::from(raw);
  EXPECT_TRUE(opt.is_some());
  EXPECT_EQ(opt.unwrap().get(), raw);

  auto empty = xpp::NonNull<void>::from(nullptr);
  EXPECT_TRUE(empty.is_none());
}

/* ── Option<NonNull<T>> construction ─────────────────────────────────── */

TEST(OptionNonNullTest, DefaultIsNone) {
  xpp::Option<xpp::NonNull<int>> o;
  EXPECT_TRUE(o.is_none());
  EXPECT_FALSE(static_cast<bool>(o));
}

TEST(OptionNonNullTest, NoneTagIsNone) {
  xpp::Option<xpp::NonNull<int>> o(xpp::none);
  EXPECT_TRUE(o.is_none());
}

TEST(OptionNonNullTest, FromNonNullIsSome) {
  int                            x = 3;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  EXPECT_TRUE(o.is_some());
  EXPECT_EQ(o.unwrap().get(), &x);
}

TEST(OptionNonNullTest, MoveCtorClearsSource) {
  int                            x = 5;
  xpp::Option<xpp::NonNull<int>> a(xpp::NonNull<int>{x});
  xpp::Option<xpp::NonNull<int>> b(std::move(a));
  EXPECT_TRUE(b.is_some());
  EXPECT_TRUE(a.is_none());
}

TEST(OptionNonNullTest, AssignNoneClears) {
  int                            x = 5;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  o = xpp::none;
  EXPECT_TRUE(o.is_none());
}

/* ── unwrap / unwrap_or / take / expect ───────────────────────────────── */

TEST(OptionNonNullTest, UnwrapHappyPath) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  EXPECT_EQ(*(o.unwrap()), 7);
}

TEST(OptionNonNullTest, UnwrapRvalueClears) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto                           p = std::move(o).unwrap();
  EXPECT_EQ(p.get(), &x);
  EXPECT_TRUE(o.is_none());
}

TEST(OptionNonNullDeathTest, UnwrapOnNoneAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Option<xpp::NonNull<int>> o;
                 (void)o.unwrap();
               }()),
               "unwrap\\(\\) on None Option");
}

TEST(OptionNonNullTest, UnwrapOrReturnsValueWhenSome) {
  int                            x = 1, fb = 99;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  EXPECT_EQ(o.unwrap_or(xpp::NonNull<int>{fb}).get(), &x);
}

TEST(OptionNonNullTest, UnwrapOrReturnsFallbackWhenNone) {
  int                            fb = 99;
  xpp::Option<xpp::NonNull<int>> o;
  EXPECT_EQ(o.unwrap_or(xpp::NonNull<int>{fb}).get(), &fb);
}

TEST(OptionNonNullTest, TakeReturnsSomeAndClears) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto                           taken = o.take();
  EXPECT_TRUE(taken.is_some());
  EXPECT_TRUE(o.is_none());
}

TEST(OptionNonNullTest, TakeOnNoneStaysNone) {
  xpp::Option<xpp::NonNull<int>> o;
  EXPECT_TRUE(o.take().is_none());
}

TEST(OptionNonNullTest, ExpectHappyPath) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  EXPECT_EQ(o.expect("must have").get(), &x);
}

TEST(OptionNonNullDeathTest, ExpectOnNoneAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Option<xpp::NonNull<int>> o;
                 (void)o.expect("missing!");
               }()),
               "missing!");
}

/* ── map ─────────────────────────────────────────────────────────────── */

TEST(OptionNonNullTest, MapAppliesWhenSome) {
  int                            x = 4;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto                           r = o.map([](xpp::NonNull<int> p) { return *p * 2; });
  EXPECT_TRUE(r.is_some());
  EXPECT_EQ(r.unwrap(), 8);
}

TEST(OptionNonNullTest, MapPassesThroughNone) {
  xpp::Option<xpp::NonNull<int>> o;
  auto                           r = o.map([](xpp::NonNull<int> p) { return *p + 1; });
  EXPECT_TRUE(r.is_none());
}

TEST(OptionNonNullTest, MapChangesType) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto                           r = o.map([](xpp::NonNull<int> p) { return std::to_string(*p); });
  static_assert(std::is_same<decltype(r), xpp::Option<std::string>>::value, "");
  EXPECT_EQ(r.unwrap(), "7");
}

/* ── and_then ─────────────────────────────────────────────────────────── */

TEST(OptionNonNullTest, AndThenChainsSome) {
  int                            x = 4;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto r = o.and_then([](xpp::NonNull<int> p) { return xpp::Option<int>(*p + 1); });
  EXPECT_EQ(r.unwrap(), 5);
}

TEST(OptionNonNullTest, AndThenReturnsNoneFromFn) {
  int                            x = 4;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto r = o.and_then([](xpp::NonNull<int>) { return xpp::Option<int>(xpp::none); });
  EXPECT_TRUE(r.is_none());
}

TEST(OptionNonNullTest, AndThenPassesThroughNone) {
  xpp::Option<xpp::NonNull<int>> o;
  bool                           called = false;
  auto                           r      = o.and_then([&](xpp::NonNull<int>) {
    called = true;
    return xpp::Option<int>(0);
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── or_else ──────────────────────────────────────────────────────────── */

TEST(OptionNonNullTest, OrElsePassesThroughSome) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  bool                           called = false;
  auto                           r      = o.or_else([&] {
    called = true;
    return xpp::Option<xpp::NonNull<int>>(xpp::none);
  });
  EXPECT_FALSE(called);
  EXPECT_EQ(r.unwrap().get(), &x);
}

TEST(OptionNonNullTest, OrElseSubstitutesOnNone) {
  int                            fb = 5;
  xpp::Option<xpp::NonNull<int>> o;
  auto r = o.or_else([&] { return xpp::Option<xpp::NonNull<int>>(xpp::NonNull<int>{fb}); });
  EXPECT_EQ(r.unwrap().get(), &fb);
}

/* ── unwrap_or_else / filter / inspect ─────────────────────────────────── */

TEST(OptionNonNullTest, UnwrapOrElseReturnsValueWhenSome) {
  int                            x = 1, fb = 99;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto p = std::move(o).unwrap_or_else([&] { return xpp::NonNull<int>{fb}; });
  EXPECT_EQ(p.get(), &x);
}

TEST(OptionNonNullTest, UnwrapOrElseCallsFnWhenNone) {
  int                            fb = 99;
  xpp::Option<xpp::NonNull<int>> o;
  auto p = std::move(o).unwrap_or_else([&] { return xpp::NonNull<int>{fb}; });
  EXPECT_EQ(p.get(), &fb);
}

TEST(OptionNonNullTest, FilterKeepsWhenPredTrue) {
  int                            x = 10;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto r = std::move(o).filter([](xpp::NonNull<int> p) { return *p > 5; });
  EXPECT_TRUE(r.is_some());
}

TEST(OptionNonNullTest, FilterDropsWhenPredFalse) {
  int                            x = 3;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  auto r = std::move(o).filter([](xpp::NonNull<int> p) { return *p > 5; });
  EXPECT_TRUE(r.is_none());
}

TEST(OptionNonNullTest, FilterOnNoneStaysNone) {
  xpp::Option<xpp::NonNull<int>> o;
  bool                           called = false;
  auto                           r      = std::move(o).filter([&](xpp::NonNull<int>) {
    called = true;
    return true;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

TEST(OptionNonNullTest, InspectCallsFnWhenSome) {
  int                            x = 7;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  int                            seen = 0;
  o.inspect([&](xpp::NonNull<int> p) { seen = *p; });
  EXPECT_EQ(seen, 7);
  EXPECT_TRUE(o.is_some());
}

TEST(OptionNonNullTest, InspectSkipsWhenNone) {
  xpp::Option<xpp::NonNull<int>> o;
  bool                           called = false;
  o.inspect([&](xpp::NonNull<int>) { called = true; });
  EXPECT_FALSE(called);
}

TEST(OptionNonNullTest, InspectIsChainable) {
  int                            x = 1;
  xpp::Option<xpp::NonNull<int>> o(xpp::NonNull<int>{x});
  int                            seen = 0;
  auto                           r =
    std::move(o).inspect([&](xpp::NonNull<int> p) { seen = *p; }).map([](xpp::NonNull<int> p) {
      return *p + 10;
    });
  EXPECT_EQ(seen, 1);
  EXPECT_EQ(r.unwrap(), 11);
}
