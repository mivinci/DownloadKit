/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * result_test.cpp - Tests for Result<T, E> and Result<void, E>.
 *
 * Covers construction, observers, unwrap variants, operator* / ->, take
 * helpers, map (lvalue + rvalue + Ok/Err propagation + type-changing),
 * and the void specialization. A Tracker helper proves no leaks or
 * double-frees in the move/take paths.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

#include <xpp/result.h>

namespace {

/**
 * @brief Lifecycle-tracking helper. See option_test.cpp for full notes.
 *
 * Defined in an anonymous namespace so each translation unit has its
 * own copy and counter set; SetUp/TearDown reset and verify per-test.
 */
struct Tracker {
  static int alive;
  static int copies;
  static int moves;

  int  value;
  bool moved_from = false;

  Tracker() : value(0) {
    ++alive;
  }
  explicit Tracker(int v) : value(v) {
    ++alive;
  }
  Tracker(const Tracker &o) : value(o.value) {
    ++alive;
    ++copies;
  }
  Tracker(Tracker &&o) noexcept : value(o.value) {
    ++alive;
    ++moves;
    o.moved_from = true;
  }
  Tracker &operator=(const Tracker &o) {
    if (this != &o) {
      value = o.value;
      ++copies;
    }
    return *this;
  }
  Tracker &operator=(Tracker &&o) noexcept {
    if (this != &o) {
      value        = o.value;
      o.moved_from = true;
      ++moves;
    }
    return *this;
  }
  ~Tracker() {
    --alive;
  }
};
int Tracker::alive  = 0;
int Tracker::copies = 0;
int Tracker::moves  = 0;

class TrackerResultTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive  = 0;
    Tracker::copies = 0;
    Tracker::moves  = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

struct Point {
  int x;
  int y;
};

} // namespace

/* ── Construction (Result<T, E>) ──────────────────────────────────────── */

TEST(ResultTest, ConstructOkAndErrSameTypeAreDistinct) {
  xpp::Result<int, int> okR(xpp::ok, 1);
  xpp::Result<int, int> errR(xpp::err, 2);

  EXPECT_TRUE(okR.isOk());
  EXPECT_FALSE(okR.isErr());
  EXPECT_EQ(okR.unwrap(), 1);

  EXPECT_FALSE(errR.isOk());
  EXPECT_TRUE(errR.isErr());
  EXPECT_EQ(errR.unwrapErr(), 2);
}

TEST(ResultTest, LvalueConstructOk) {
  std::string                   s = "ok-value";
  xpp::Result<std::string, int> r(xpp::ok, s);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.unwrap(), "ok-value");
  EXPECT_EQ(s, "ok-value"); // s not moved-from
}

TEST(ResultTest, RvalueConstructOk) {
  xpp::Result<std::string, int> r(xpp::ok, std::string("moved"));
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.unwrap(), "moved");
}

TEST(ResultTest, LvalueConstructErr) {
  std::string                   e = "err-msg";
  xpp::Result<int, std::string> r(xpp::err, e);
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.unwrapErr(), "err-msg");
}

TEST(ResultTest, RvalueConstructErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("moved-err"));
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.unwrapErr(), "moved-err");
}

TEST_F(TrackerResultTest, ConstructOkWithTrackerMoves) {
  {
    xpp::Result<Tracker, std::string> r(xpp::ok, Tracker(11));
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(Tracker::copies, 0);
    EXPECT_GE(Tracker::moves, 1);
    EXPECT_EQ(r.unwrap().value, 11);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerResultTest, ConstructErrWithTrackerMoves) {
  {
    xpp::Result<int, Tracker> r(xpp::err, Tracker(22));
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(Tracker::copies, 0);
    EXPECT_GE(Tracker::moves, 1);
    EXPECT_EQ(r.unwrapErr().value, 22);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Observers ────────────────────────────────────────────────────────── */

TEST(ResultTest, ObserverConsistencyOk) {
  xpp::Result<int, int> r(xpp::ok, 1);
  EXPECT_TRUE(r.isOk());
  EXPECT_FALSE(r.isErr());
  EXPECT_TRUE(static_cast<bool>(r));
}

TEST(ResultTest, ObserverConsistencyErr) {
  xpp::Result<int, int> r(xpp::err, 1);
  EXPECT_FALSE(r.isOk());
  EXPECT_TRUE(r.isErr());
  EXPECT_FALSE(static_cast<bool>(r));
}

TEST(ResultTest, UnwrapReturnsMutableReference) {
  xpp::Result<int, int> r(xpp::ok, 7);
  r.unwrap() = 8;
  EXPECT_EQ(r.unwrap(), 8);
}

TEST(ResultTest, UnwrapConstOverload) {
  const xpp::Result<int, int> r(xpp::ok, 9);
  EXPECT_EQ(r.unwrap(), 9);
}

TEST_F(TrackerResultTest, UnwrapRvalueOverloadMoves) {
  xpp::Result<Tracker, int> r(xpp::ok, Tracker(15));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker t       = std::move(r).unwrap();
  EXPECT_EQ(t.value, 15);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_GE(Tracker::moves, 1);
}

TEST(ResultDeathTest, UnwrapOnErr) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<int, int> r(xpp::err, 7);
                 (void)r.unwrap();
               }()),
               "unwrap\\(\\) on Err Result");
}

TEST(ResultTest, UnwrapErrConstOverload) {
  const xpp::Result<int, int> r(xpp::err, 9);
  EXPECT_EQ(r.unwrapErr(), 9);
}

TEST_F(TrackerResultTest, UnwrapErrRvalueOverloadMoves) {
  xpp::Result<int, Tracker> r(xpp::err, Tracker(33));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker t       = std::move(r).unwrapErr();
  EXPECT_EQ(t.value, 33);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_GE(Tracker::moves, 1);
}

TEST(ResultDeathTest, UnwrapErrOnOk) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<int, int> r(xpp::ok, 7);
                 (void)r.unwrapErr();
               }()),
               "unwrapErr\\(\\) on Ok Result");
}

TEST(ResultTest, UnwrapUncheckedHappyPath) {
  xpp::Result<int, int> r(xpp::ok, 11);
  EXPECT_EQ(r.unwrapUnchecked(), 11);
  const xpp::Result<int, int> cr(xpp::ok, 12);
  EXPECT_EQ(cr.unwrapUnchecked(), 12);
}

#ifndef NDEBUG
TEST(ResultDeathTest, UnwrapUncheckedOnErrInDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<int, int> r(xpp::err, 7);
                 (void)r.unwrapUnchecked();
               }()),
               "internal: Result must be Ok");
}
#endif

TEST(ResultTest, UnwrapErrUncheckedHappyPath) {
  xpp::Result<int, int> r(xpp::err, 11);
  EXPECT_EQ(r.unwrapErrUnchecked(), 11);
}

#ifndef NDEBUG
TEST(ResultDeathTest, UnwrapErrUncheckedOnOkInDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<int, int> r(xpp::ok, 7);
                 (void)r.unwrapErrUnchecked();
               }()),
               "internal: Result must be Err");
}
#endif

TEST(ResultTest, UnwrapOrOkReturnsHeld) {
  xpp::Result<int, int> r(xpp::ok, 7);
  EXPECT_EQ(r.unwrapOr(99), 7);
}

TEST(ResultTest, UnwrapOrErrReturnsFallback) {
  xpp::Result<int, int> r(xpp::err, 1);
  EXPECT_EQ(r.unwrapOr(99), 99);
}

TEST_F(TrackerResultTest, UnwrapOrRvalueOverloadMovesFallback) {
  xpp::Result<Tracker, int> r(xpp::err, 1);
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker fallback(42);
  Tracker out = std::move(r).unwrapOr(std::move(fallback));
  EXPECT_EQ(out.value, 42);
  EXPECT_EQ(Tracker::copies, 0);
}

/* ── operator* / operator-> ──────────────────────────────────────────── */

TEST(ResultTest, DereferenceReturnsOkValue) {
  xpp::Result<int, int> r(xpp::ok, 41);
  EXPECT_EQ(*r, 41);
  *r = 42;
  EXPECT_EQ(*r, 42);
}

TEST(ResultTest, DereferenceConstOverload) {
  const xpp::Result<int, int> r(xpp::ok, 7);
  EXPECT_EQ(*r, 7);
}

TEST_F(TrackerResultTest, DereferenceRvalueOverloadMoves) {
  xpp::Result<Tracker, int> r(xpp::ok, Tracker(50));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker t       = *std::move(r);
  EXPECT_EQ(t.value, 50);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_GE(Tracker::moves, 1);
}

TEST(ResultTest, ArrowOperatorAccessesMember) {
  xpp::Result<Point, int> r(xpp::ok, Point{3, 4});
  EXPECT_EQ(r->x, 3);
  EXPECT_EQ(r->y, 4);
  r->x = 10;
  EXPECT_EQ(r->x, 10);

  const xpp::Result<Point, int> &cr = r;
  EXPECT_EQ(cr->x, 10);
}

/* ── ok() / err() consume helpers ─────────────────────────────────────── */

TEST_F(TrackerResultTest, OkOnOkReturnsSome) {
  xpp::Result<Tracker, int> r(xpp::ok, Tracker(60));
  Tracker::copies        = 0;
  xpp::Option<Tracker> o = std::move(r).ok();
  EXPECT_TRUE(o.isSome());
  EXPECT_EQ(o.unwrap().value, 60);
  EXPECT_EQ(Tracker::copies, 0);
}

TEST(ResultTest, OkOnErrReturnsNone) {
  xpp::Result<int, std::string> r(xpp::err, "boom");
  xpp::Option<int>              o = std::move(r).ok();
  EXPECT_TRUE(o.isNone());
}

TEST_F(TrackerResultTest, ErrOnErrReturnsSome) {
  xpp::Result<int, Tracker> r(xpp::err, Tracker(70));
  Tracker::copies        = 0;
  xpp::Option<Tracker> o = std::move(r).err();
  EXPECT_TRUE(o.isSome());
  EXPECT_EQ(o.unwrap().value, 70);
  EXPECT_EQ(Tracker::copies, 0);
}

TEST(ResultTest, ErrOnOkReturnsNone) {
  xpp::Result<int, std::string> r(xpp::ok, 42);
  xpp::Option<std::string>      o = std::move(r).err();
  EXPECT_TRUE(o.isNone());
}

/* ── transpose ────────────────────────────────────────────────────────── */

TEST(ResultTransposeTest, OkSomeBecomesSomeOk) {
  xpp::Result<xpp::Option<int>, std::string> r(xpp::ok, xpp::Some(7));
  auto                                       out = std::move(r).transpose();
  ASSERT_TRUE(out.isSome());
  EXPECT_TRUE(out.unwrap().isOk());
  EXPECT_EQ(out.unwrap().unwrap(), 7);
}

TEST(ResultTransposeTest, OkNoneBecomesNone) {
  xpp::Result<xpp::Option<int>, std::string> r(xpp::ok, xpp::Option<int>(xpp::none));
  auto                                       out = std::move(r).transpose();
  EXPECT_TRUE(out.isNone());
}

TEST(ResultTransposeTest, ErrBecomesSomeErr) {
  xpp::Result<xpp::Option<int>, std::string> r(xpp::err, std::string("boom"));
  auto                                       out = std::move(r).transpose();
  ASSERT_TRUE(out.isSome());
  EXPECT_TRUE(out.unwrap().isErr());
  EXPECT_EQ(out.unwrap().unwrapErr(), "boom");
}

TEST_F(TrackerResultTest, TransposeMovesValueZeroCopies) {
  xpp::Result<xpp::Option<Tracker>, int> r(xpp::ok, xpp::Some(Tracker(81)));
  Tracker::copies = 0;
  auto out        = std::move(r).transpose();
  ASSERT_TRUE(out.isSome());
  EXPECT_EQ(out.unwrap().unwrap().value, 81);
  EXPECT_EQ(Tracker::copies, 0);
}

/*
 * SFINAE check: Result<NonOption, E> must NOT have a callable transpose().
 * Detect via expression SFINAE and assert at compile time. If transpose ever
 * leaks onto a non-Option payload, this static_assert breaks the build.
 */
namespace {
template <class, typename = void> struct has_transpose : std::false_type {};
template <class R>
struct has_transpose<R, decltype(void(std::declval<R>().transpose()))> : std::true_type {};

static_assert(has_transpose<xpp::Result<xpp::Option<int>, std::string>>::value,
              "Result<Option<T>, E> must have transpose()");
static_assert(!has_transpose<xpp::Result<int, std::string>>::value,
              "Result<int, E> must NOT have transpose()");
static_assert(!has_transpose<xpp::Result<std::string, int>>::value,
              "Result<string, E> must NOT have transpose()");
} // namespace

/* ── map ──────────────────────────────────────────────────────────────── */

TEST(ResultTest, MapConstLvalueOnOk) {
  xpp::Result<int, std::string> r(xpp::ok, 41);
  auto                          s = r.map([](int x) { return x + 1; });
  static_assert(std::is_same<decltype(s), xpp::Result<int, std::string>>::value,
                "map must preserve E");
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), 42);
  // Original is untouched.
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.unwrap(), 41);
}

TEST(ResultTest, MapConstLvalueOnErrPropagatesUnchanged) {
  xpp::Result<int, std::string> r(xpp::err, "bang");
  auto                          s = r.map([](int x) { return x + 1; });
  EXPECT_TRUE(s.isErr());
  EXPECT_EQ(s.unwrapErr(), "bang");
}

TEST_F(TrackerResultTest, MapRvalueOnOkMovesValue) {
  xpp::Result<Tracker, int> r(xpp::ok, Tracker(80));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  auto s          = std::move(r).map([](Tracker &&t) { return Tracker(t.value + 1); });
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap().value, 81);
  EXPECT_EQ(Tracker::copies, 0);
}

TEST_F(TrackerResultTest, MapRvalueOnErrMovesError) {
  xpp::Result<int, Tracker> r(xpp::err, Tracker(90));
  Tracker::copies = 0;
  auto s          = std::move(r).map([](int x) { return x + 1; });
  EXPECT_TRUE(s.isErr());
  EXPECT_EQ(s.unwrapErr().value, 90);
  EXPECT_EQ(Tracker::copies, 0);
}

TEST(ResultTest, MapTypeChange) {
  xpp::Result<int, std::string> r(xpp::ok, 5);
  auto                          s = r.map([](int x) { return std::string(x, 'a'); });
  static_assert(std::is_same<decltype(s), xpp::Result<std::string, std::string>>::value,
                "map must allow changing T");
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), "aaaaa");
}

/* ── Result<void, E> specialization ──────────────────────────────────── */

TEST(ResultVoidTest, ConstructOk) {
  xpp::Result<void, int> r(xpp::ok);
  EXPECT_TRUE(r.isOk());
  EXPECT_FALSE(r.isErr());
  EXPECT_TRUE(static_cast<bool>(r));
}

TEST(ResultVoidTest, ConstructErr) {
  xpp::Result<void, int> r(xpp::err, 5);
  EXPECT_FALSE(r.isOk());
  EXPECT_TRUE(r.isErr());
  EXPECT_FALSE(static_cast<bool>(r));
  EXPECT_EQ(r.unwrapErr(), 5);
}

TEST(ResultVoidTest, ConstructErrRvalue) {
  xpp::Result<void, std::string> r(xpp::err, std::string("oops"));
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.unwrapErr(), "oops");
}

TEST(ResultVoidTest, UnwrapErrConstOverload) {
  const xpp::Result<void, int> r(xpp::err, 7);
  EXPECT_EQ(r.unwrapErr(), 7);
}

TEST_F(TrackerResultTest, UnwrapErrRvalueVoidMoves) {
  xpp::Result<void, Tracker> r(xpp::err, Tracker(101));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker t       = std::move(r).unwrapErr();
  EXPECT_EQ(t.value, 101);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_GE(Tracker::moves, 1);
}

TEST(ResultVoidDeathTest, UnwrapErrOnOk) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<void, int> r(xpp::ok);
                 (void)r.unwrapErr();
               }()),
               "unwrapErr\\(\\) on Ok Result");
}

TEST(ResultVoidTest, UnwrapErrUncheckedHappyPath) {
  xpp::Result<void, int> r(xpp::err, 13);
  EXPECT_EQ(r.unwrapErrUnchecked(), 13);
}

#ifndef NDEBUG
TEST(ResultVoidDeathTest, UnwrapErrUncheckedOnOkInDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<void, int> r(xpp::ok);
                 (void)r.unwrapErrUnchecked();
               }()),
               "internal: Result must be Err");
}
#endif

TEST_F(TrackerResultTest, ErrOnVoidErr) {
  xpp::Result<void, Tracker> r(xpp::err, Tracker(110));
  Tracker::copies        = 0;
  xpp::Option<Tracker> o = std::move(r).err();
  EXPECT_TRUE(o.isSome());
  EXPECT_EQ(o.unwrap().value, 110);
  EXPECT_EQ(Tracker::copies, 0);
}

TEST(ResultVoidTest, ErrOnVoidOkReturnsNone) {
  xpp::Result<void, std::string> r(xpp::ok);
  xpp::Option<std::string>       o = std::move(r).err();
  EXPECT_TRUE(o.isNone());
}

/* ── Edge / quality ───────────────────────────────────────────────────── */

TEST_F(TrackerResultTest, DestructorRunsForOkValue) {
  {
    xpp::Result<Tracker, int> r(xpp::ok, Tracker(120));
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerResultTest, DestructorRunsForErrValue) {
  {
    xpp::Result<int, Tracker> r(xpp::err, Tracker(121));
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── expect ───────────────────────────────────────────────────────────── */

TEST(ResultTest, ExpectReturnsValueWhenOk) {
  xpp::Result<int, std::string> r(xpp::ok, 42);
  EXPECT_EQ(r.expect("must be ok"), 42);
}

TEST(ResultTest, ExpectConstRefReturnsValue) {
  const xpp::Result<int, std::string> r(xpp::ok, 42);
  EXPECT_EQ(r.expect("must be ok"), 42);
}

TEST(ResultTest, ExpectRvalueMoves) {
  xpp::Result<std::string, int> r(xpp::ok, std::string("hi"));
  std::string                   s = std::move(r).expect("must be ok");
  EXPECT_EQ(s, "hi");
}

TEST(ResultDeathTest, ExpectOnErrAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<int, std::string> r(xpp::err, std::string("boom"));
                 (void)r.expect("must be ok");
               }()),
               "must be ok");
}

/* ── expectErr ────────────────────────────────────────────────────────── */

TEST(ResultTest, ExpectErrReturnsErrWhenErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  EXPECT_EQ(r.expectErr("must be err"), "bad");
}

TEST(ResultTest, ExpectErrRvalueMoves) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  std::string                   s = std::move(r).expectErr("must be err");
  EXPECT_EQ(s, "bad");
}

TEST(ResultDeathTest, ExpectErrOnOkAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<int, std::string> r(xpp::ok, 1);
                 (void)r.expectErr("must be err");
               }()),
               "must be err");
}

TEST(ResultVoidTest, ExpectErrReturnsErrWhenErr) {
  xpp::Result<void, std::string> r(xpp::err, std::string("bad"));
  EXPECT_EQ(r.expectErr("must be err"), "bad");
}

TEST(ResultVoidDeathTest, ExpectErrOnOkAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Result<void, std::string> r(xpp::ok);
                 (void)r.expectErr("must be err");
               }()),
               "must be err");
}

/* ── mapErr ───────────────────────────────────────────────────────────── */

TEST(ResultTest, MapErrTransformsErr) {
  xpp::Result<int, int> r(xpp::err, 7);
  auto                  s = r.mapErr([](int x) { return x + 100; });
  EXPECT_TRUE(s.isErr());
  EXPECT_EQ(s.unwrapErr(), 107);
}

TEST(ResultTest, MapErrPassesThroughOk) {
  xpp::Result<int, int> r(xpp::ok, 1);
  bool                  called = false;
  auto                  s      = r.mapErr([&](int x) {
    called = true;
    return x;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), 1);
}

TEST(ResultTest, MapErrChangesErrType) {
  xpp::Result<int, int> r(xpp::err, 9);
  auto                  s = r.mapErr([](int x) { return std::to_string(x); });
  static_assert(std::is_same<decltype(s), xpp::Result<int, std::string>>::value, "");
  EXPECT_EQ(s.unwrapErr(), "9");
}

TEST(ResultVoidTest, MapErrOnVoid) {
  xpp::Result<void, int> r(xpp::err, 3);
  auto                   s = r.mapErr([](int x) { return std::to_string(x); });
  static_assert(std::is_same<decltype(s), xpp::Result<void, std::string>>::value, "");
  EXPECT_EQ(s.unwrapErr(), "3");
}

TEST_F(TrackerResultTest, MapErrRvalueMovesErr) {
  {
    xpp::Result<int, Tracker> r(xpp::err, Tracker(5));
    Tracker::copies = 0;
    auto s          = std::move(r).mapErr([](Tracker &&t) { return Tracker(t.value + 1); });
    EXPECT_EQ(Tracker::copies, 0);
    EXPECT_EQ(s.unwrapErr().value, 6);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── andThen ──────────────────────────────────────────────────────────── */

TEST(ResultTest, AndThenChainsOk) {
  xpp::Result<int, std::string> r(xpp::ok, 4);
  auto s = r.andThen([](int x) { return xpp::Result<int, std::string>(xpp::ok, x + 1); });
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), 5);
}

TEST(ResultTest, AndThenPassesThroughErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("nope"));
  bool                          called = false;
  auto                          s      = r.andThen([&](int x) {
    called = true;
    return xpp::Result<int, std::string>(xpp::ok, x);
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(s.isErr());
  EXPECT_EQ(s.unwrapErr(), "nope");
}

TEST(ResultTest, AndThenFnCanReturnErr) {
  xpp::Result<int, std::string> r(xpp::ok, 4);
  auto s = r.andThen([](int) { return xpp::Result<int, std::string>(xpp::err, "bad"); });
  EXPECT_TRUE(s.isErr());
  EXPECT_EQ(s.unwrapErr(), "bad");
}

TEST(ResultTest, AndThenChangesOkType) {
  xpp::Result<int, std::string> r(xpp::ok, 7);
  auto                          s = r.andThen(
    [](int x) { return xpp::Result<std::string, std::string>(xpp::ok, std::to_string(x)); });
  static_assert(std::is_same<decltype(s), xpp::Result<std::string, std::string>>::value, "");
  EXPECT_EQ(s.unwrap(), "7");
}

TEST(ResultVoidTest, AndThenOnVoidOk) {
  xpp::Result<void, std::string> r(xpp::ok);
  auto s = r.andThen([] { return xpp::Result<int, std::string>(xpp::ok, 9); });
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), 9);
}

TEST(ResultVoidTest, AndThenOnVoidErrPropagates) {
  xpp::Result<void, std::string> r(xpp::err, std::string("nope"));
  bool                           called = false;
  auto                           s      = r.andThen([&] {
    called = true;
    return xpp::Result<int, std::string>(xpp::ok, 9);
  });
  EXPECT_FALSE(called);
  EXPECT_EQ(s.unwrapErr(), "nope");
}

/* ── orElse ───────────────────────────────────────────────────────────── */

TEST(ResultTest, OrElsePassesThroughOk) {
  xpp::Result<int, std::string> r(xpp::ok, 7);
  bool                          called = false;
  auto                          s      = r.orElse([&](const std::string &) {
    called = true;
    return xpp::Result<int, int>(xpp::err, 99);
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), 7);
}

TEST(ResultTest, OrElseRecoversFromErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  auto                          s = r.orElse([](const std::string &e) {
    return xpp::Result<int, int>(xpp::ok, static_cast<int>(e.size()));
  });
  EXPECT_TRUE(s.isOk());
  EXPECT_EQ(s.unwrap(), 3);
}

TEST(ResultTest, OrElseFnCanReturnErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  auto s = r.orElse([](const std::string &) { return xpp::Result<int, int>(xpp::err, 42); });
  EXPECT_TRUE(s.isErr());
  EXPECT_EQ(s.unwrapErr(), 42);
}

TEST(ResultVoidTest, OrElseVoidRecovers) {
  xpp::Result<void, int> r(xpp::err, 5);
  auto                   s = r.orElse([](int x) {
    return x > 0 ? xpp::Result<void, int>(xpp::ok) : xpp::Result<void, int>(xpp::err, -1);
  });
  EXPECT_TRUE(s.isOk());
}

/* ── unwrapOrElse ─────────────────────────────────────────────────────── */

TEST(ResultTest, UnwrapOrElseReturnsValueWhenOk) {
  xpp::Result<int, std::string> r(xpp::ok, 5);
  EXPECT_EQ(std::move(r).unwrapOrElse([](std::string &&) { return 99; }), 5);
}

TEST(ResultTest, UnwrapOrElseUsesErrWhenErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("xyz"));
  int v = std::move(r).unwrapOrElse([](std::string &&e) { return static_cast<int>(e.size()); });
  EXPECT_EQ(v, 3);
}

/* ── inspect ──────────────────────────────────────────────────────────── */

TEST(ResultTest, InspectCallsFnWhenOk) {
  xpp::Result<int, std::string> r(xpp::ok, 7);
  int                           seen = 0;
  r.inspect([&](const int &x) { seen = x; });
  EXPECT_EQ(seen, 7);
  EXPECT_TRUE(r.isOk());
}

TEST(ResultTest, InspectSkipsWhenErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  bool                          called = false;
  r.inspect([&](const int &) { called = true; });
  EXPECT_FALSE(called);
}

TEST(ResultTest, InspectIsChainable) {
  xpp::Result<int, std::string> r(xpp::ok, 1);
  int                           seen = 0;
  auto s = std::move(r).inspect([&](const int &x) { seen = x; }).map([](int x) { return x + 10; });
  EXPECT_EQ(seen, 1);
  EXPECT_EQ(s.unwrap(), 11);
}

/* ── inspectErr ───────────────────────────────────────────────────────── */

TEST(ResultTest, InspectErrCallsFnWhenErr) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  std::string                   seen;
  r.inspectErr([&](const std::string &e) { seen = e; });
  EXPECT_EQ(seen, "bad");
  EXPECT_TRUE(r.isErr());
}

TEST(ResultTest, InspectErrSkipsWhenOk) {
  xpp::Result<int, std::string> r(xpp::ok, 1);
  bool                          called = false;
  r.inspectErr([&](const std::string &) { called = true; });
  EXPECT_FALSE(called);
}

TEST(ResultTest, InspectErrIsChainable) {
  xpp::Result<int, std::string> r(xpp::err, std::string("bad"));
  std::string                   seen;
  auto                          s =
    std::move(r).inspectErr([&](const std::string &e) { seen = e; }).mapErr([](std::string &&e) {
      return e + "!";
    });
  EXPECT_EQ(seen, "bad");
  EXPECT_EQ(s.unwrapErr(), "bad!");
}

TEST(ResultVoidTest, InspectErrOnVoid) {
  xpp::Result<void, int> r(xpp::err, 9);
  int                    seen = 0;
  r.inspectErr([&](const int &e) { seen = e; });
  EXPECT_EQ(seen, 9);
}
