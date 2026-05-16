/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * option_test.cpp - Tests for Option<T>.
 *
 * Covers construction, copy/move, observers, unwrap variants, take(),
 * and destructor correctness. A Tracker helper + TrackerTest fixture
 * ensure no leaks or double-frees slip past the tests.
 */

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "option.h"
#include "result.h"

namespace {

/**
 * @brief Lifecycle-tracking helper for Option storage tests.
 *
 * Counters are reset by TrackerTest::SetUp and verified by TearDown.
 * Tests that don't need lifecycle proof (pure semantic checks) can
 * use plain TEST(...) with int / std::string instead.
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

class TrackerTest : public ::testing::Test {
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

} // namespace

/* ── Construction ─────────────────────────────────────────────────────── */

TEST(OptionTest, DefaultConstructIsNone) {
  xpp::Option<int> o;
  EXPECT_TRUE(o.isNone());
  EXPECT_FALSE(o.isSome());
  EXPECT_FALSE(static_cast<bool>(o));
}

TEST(OptionTest, NoneTagConstructIsNone) {
  xpp::Option<int> o(xpp::none);
  EXPECT_TRUE(o.isNone());
}

TEST(OptionTest, ValueConstructIsSome) {
  xpp::Option<int> o(42);
  EXPECT_TRUE(o.isSome());
  EXPECT_EQ(o.unwrap(), 42);
}

TEST(OptionTest, RvalueConstruct) {
  std::string       s = "hi";
  xpp::Option<std::string> o(std::move(s));
  EXPECT_TRUE(o.isSome());
  EXPECT_EQ(o.unwrap(), "hi");
}

TEST(OptionTest, SomeFactoryDeducesType) {
  auto o = xpp::Some(42);
  static_assert(std::is_same<decltype(o), xpp::Option<int>>::value,
                "Some(int) must deduce Option<int>");
  EXPECT_EQ(o.unwrap(), 42);
}

TEST(OptionTest, SomeFactoryMovesRvalue) {
  auto o = xpp::Some(std::string("hello"));
  static_assert(std::is_same<decltype(o), xpp::Option<std::string>>::value,
                "Some(string&&) must deduce Option<string>");
  EXPECT_EQ(o.unwrap(), "hello");
}

TEST_F(TrackerTest, ValueConstructMovesNotCopies) {
  {
    xpp::Option<Tracker> o(Tracker(7));
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(Tracker::copies, 0);
    EXPECT_GE(Tracker::moves, 1); // at least one move (temporary into storage)
    EXPECT_EQ(o.unwrap().value, 7);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Copy / move ──────────────────────────────────────────────────────── */

TEST_F(TrackerTest, CopyOfSome) {
  xpp::Option<Tracker> a(Tracker(1));
  Tracker::copies = 0;
  xpp::Option<Tracker> b(a);
  EXPECT_TRUE(a.isSome());
  EXPECT_TRUE(b.isSome());
  EXPECT_EQ(a.unwrap().value, 1);
  EXPECT_EQ(b.unwrap().value, 1);
  EXPECT_EQ(Tracker::copies, 1);
}

TEST_F(TrackerTest, CopyOfNone) {
  xpp::Option<Tracker> a;
  xpp::Option<Tracker> b(a);
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isNone());
  EXPECT_EQ(Tracker::alive, 0);
  EXPECT_EQ(Tracker::copies, 0);
}

TEST_F(TrackerTest, MoveOfSomeLeavesSourceNone) {
  xpp::Option<Tracker> a(Tracker(2));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  xpp::Option<Tracker> b(std::move(a));
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isSome());
  EXPECT_EQ(b.unwrap().value, 2);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_GE(Tracker::moves, 1);
}

TEST_F(TrackerTest, MoveOfNone) {
  xpp::Option<Tracker> a;
  xpp::Option<Tracker> b(std::move(a));
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isNone());
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerTest, SelfCopyAssignNoOp) {
  xpp::Option<Tracker> a(Tracker(3));
  // Through a reference to defeat -Wself-assign-overloaded.
  xpp::Option<Tracker> &ref = a;
  a = ref;
  EXPECT_TRUE(a.isSome());
  EXPECT_EQ(a.unwrap().value, 3);
  EXPECT_EQ(Tracker::alive, 1);
}

TEST_F(TrackerTest, SelfMoveAssignNoOp) {
  xpp::Option<Tracker> a(Tracker(4));
  xpp::Option<Tracker> &ref = a;
  a = std::move(ref);
  // After self-move semantics are valid but unspecified; we only assert
  // no leak/double-free.
  EXPECT_LE(Tracker::alive, 1);
}

TEST_F(TrackerTest, CopyAssignSomeToSomeDestroysOldValue) {
  xpp::Option<Tracker> a(Tracker(10));
  xpp::Option<Tracker> b(Tracker(20));
  a = b;
  EXPECT_EQ(a.unwrap().value, 20);
  EXPECT_EQ(b.unwrap().value, 20);
  EXPECT_EQ(Tracker::alive, 2); // one in a, one in b
}

TEST_F(TrackerTest, CopyAssignNoneToSome) {
  xpp::Option<Tracker> a;
  xpp::Option<Tracker> b(Tracker(5));
  a = b;
  EXPECT_TRUE(a.isSome());
  EXPECT_EQ(a.unwrap().value, 5);
  EXPECT_EQ(Tracker::alive, 2);
}

TEST_F(TrackerTest, CopyAssignSomeToNone) {
  xpp::Option<Tracker> a(Tracker(5));
  xpp::Option<Tracker> b;
  a = b;
  EXPECT_TRUE(a.isNone());
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerTest, CopyAssignNoneToNone) {
  xpp::Option<Tracker> a;
  xpp::Option<Tracker> b;
  a = b;
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isNone());
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerTest, MoveAssignSomeToSome) {
  xpp::Option<Tracker> a(Tracker(10));
  xpp::Option<Tracker> b(Tracker(20));
  a = std::move(b);
  EXPECT_EQ(a.unwrap().value, 20);
  EXPECT_TRUE(b.isNone()); // source becomes None
  EXPECT_EQ(Tracker::alive, 1);
}

TEST_F(TrackerTest, MoveAssignNoneToSome) {
  xpp::Option<Tracker> a;
  xpp::Option<Tracker> b(Tracker(5));
  a = std::move(b);
  EXPECT_TRUE(a.isSome());
  EXPECT_EQ(a.unwrap().value, 5);
  EXPECT_TRUE(b.isNone());
  EXPECT_EQ(Tracker::alive, 1);
}

TEST_F(TrackerTest, MoveAssignSomeToNone) {
  xpp::Option<Tracker> a(Tracker(5));
  xpp::Option<Tracker> b;
  a = std::move(b);
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isNone());
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerTest, MoveAssignNoneToNone) {
  xpp::Option<Tracker> a;
  xpp::Option<Tracker> b;
  a = std::move(b);
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isNone());
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerTest, AssignNoneTagClearsSome) {
  xpp::Option<Tracker> a(Tracker(99));
  EXPECT_EQ(Tracker::alive, 1);
  a = xpp::none;
  EXPECT_TRUE(a.isNone());
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Observers ────────────────────────────────────────────────────────── */

TEST(OptionTest, ObserverConsistency) {
  xpp::Option<int> some(1);
  EXPECT_TRUE(some.isSome());
  EXPECT_FALSE(some.isNone());
  EXPECT_TRUE(static_cast<bool>(some));

  xpp::Option<int> nothing;
  EXPECT_FALSE(nothing.isSome());
  EXPECT_TRUE(nothing.isNone());
  EXPECT_FALSE(static_cast<bool>(nothing));
}

TEST(OptionTest, UnwrapReturnsMutableReference) {
  xpp::Option<int> o(7);
  o.unwrap() = 8;
  EXPECT_EQ(o.unwrap(), 8);
}

TEST(OptionTest, UnwrapConstOverload) {
  const xpp::Option<int> o(7);
  EXPECT_EQ(o.unwrap(), 7);
}

TEST_F(TrackerTest, UnwrapRvalueOverloadMoves) {
  xpp::Option<Tracker> o(Tracker(5));
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker t = std::move(o).unwrap();
  EXPECT_EQ(t.value, 5);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_GE(Tracker::moves, 1);
}

TEST(OptionDeathTest, UnwrapOnNone) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        xpp::Option<int> o;
        (void)o.unwrap();
      },
      "unwrap\\(\\) on None Option");
}

TEST(OptionTest, UnwrapUncheckedHappyPath) {
  xpp::Option<int> o(11);
  EXPECT_EQ(o.unwrapUnchecked(), 11);
  const xpp::Option<int> co(12);
  EXPECT_EQ(co.unwrapUnchecked(), 12);
}

#ifndef NDEBUG
TEST(OptionDeathTest, UnwrapUncheckedOnNoneInDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        xpp::Option<int> o;
        (void)o.unwrapUnchecked();
      },
      "internal: Option must be Some");
}
#endif

TEST(OptionTest, UnwrapOrSomeReturnsHeld) {
  xpp::Option<int> o(7);
  EXPECT_EQ(o.unwrapOr(99), 7);
}

TEST(OptionTest, UnwrapOrNoneReturnsFallback) {
  xpp::Option<int> o;
  EXPECT_EQ(o.unwrapOr(99), 99);
}

TEST_F(TrackerTest, UnwrapOrRvalueOverloadMovesFallback) {
  xpp::Option<Tracker> o; // None
  Tracker::copies = 0;
  Tracker::moves  = 0;
  Tracker fallback(42);
  Tracker out = std::move(o).unwrapOr(std::move(fallback));
  EXPECT_EQ(out.value, 42);
  EXPECT_EQ(Tracker::copies, 0);
}

/* ── take() ───────────────────────────────────────────────────────────── */

TEST_F(TrackerTest, TakeOnSomeReturnsValueAndClears) {
  xpp::Option<Tracker> a(Tracker(50));
  Tracker::copies = 0;
  xpp::Option<Tracker> b = a.take();
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isSome());
  EXPECT_EQ(b.unwrap().value, 50);
  EXPECT_EQ(Tracker::copies, 0);
  EXPECT_EQ(Tracker::alive, 1);
}

TEST(OptionTest, TakeOnNoneReturnsNone) {
  xpp::Option<int> a;
  xpp::Option<int> b = a.take();
  EXPECT_TRUE(a.isNone());
  EXPECT_TRUE(b.isNone());
}

TEST_F(TrackerTest, TakeNoLeakOrDoubleFree) {
  {
    xpp::Option<Tracker> a(Tracker(1));
    xpp::Option<Tracker> b = a.take();
    EXPECT_TRUE(a.isNone());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Edge cases ───────────────────────────────────────────────────────── */

TEST_F(TrackerTest, DestructorRunsForHeldValue) {
  {
    xpp::Option<Tracker> o(Tracker(123));
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(TrackerTest, MoveFromNoneConstructsNothing) {
  xpp::Option<Tracker> a;
  EXPECT_EQ(Tracker::alive, 0);
  xpp::Option<Tracker> b(std::move(a));
  EXPECT_EQ(Tracker::alive, 0);
  EXPECT_TRUE(b.isNone());
}

namespace {
struct NoDefault {
  int v;
  explicit NoDefault(int x) : v(x) {}
  NoDefault()                  = delete;
};
} // namespace

TEST(OptionTest, WorksWithNonDefaultConstructibleType) {
  xpp::Option<NoDefault> o(NoDefault(7));
  EXPECT_TRUE(o.isSome());
  EXPECT_EQ(o.unwrap().v, 7);

  xpp::Option<NoDefault> empty;
  EXPECT_TRUE(empty.isNone());
}

/* ── expect ───────────────────────────────────────────────────────────── */

TEST(OptionTest, ExpectReturnsValueWhenSome) {
  xpp::Option<int> o(42);
  EXPECT_EQ(o.expect("must have value"), 42);
}

TEST(OptionTest, ExpectConstRefReturnsValueWhenSome) {
  const xpp::Option<int> o(42);
  EXPECT_EQ(o.expect("must have value"), 42);
}

TEST(OptionTest, ExpectRvalueReturnsValueWhenSome) {
  xpp::Option<std::string> o(std::string("hi"));
  std::string              s = std::move(o).expect("must have value");
  EXPECT_EQ(s, "hi");
}

TEST(OptionDeathTest, ExpectOnNoneAborts) {
  xpp::Option<int> o;
  EXPECT_DEATH({ (void)o.expect("missing!"); }, "missing!");
}

/* ── map ──────────────────────────────────────────────────────────────── */

TEST(OptionTest, MapAppliesFunctionWhenSome) {
  xpp::Option<int> o(3);
  auto             r = o.map([](int x) { return x * 2; });
  EXPECT_TRUE(r.isSome());
  EXPECT_EQ(r.unwrap(), 6);
}

TEST(OptionTest, MapPassesThroughNone) {
  xpp::Option<int> o;
  auto             r = o.map([](int x) { return x * 2; });
  EXPECT_TRUE(r.isNone());
}

TEST(OptionTest, MapChangesType) {
  xpp::Option<int> o(7);
  auto             r = o.map([](int x) { return std::to_string(x); });
  static_assert(std::is_same<decltype(r), xpp::Option<std::string>>::value, "");
  EXPECT_EQ(r.unwrap(), "7");
}

TEST_F(TrackerTest, MapRvalueMovesValueIntoFn) {
  {
    xpp::Option<Tracker> o(Tracker(5));
    Tracker::copies = 0;
    Tracker::moves  = 0;
    auto r          = std::move(o).map([](Tracker &&t) { return Tracker(t.value + 1); });
    EXPECT_EQ(Tracker::copies, 0);
    EXPECT_TRUE(r.isSome());
    EXPECT_EQ(r.unwrap().value, 6);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── andThen ──────────────────────────────────────────────────────────── */

TEST(OptionTest, AndThenChainsSome) {
  xpp::Option<int> o(4);
  auto r = o.andThen([](int x) { return xpp::Option<int>(x + 1); });
  EXPECT_TRUE(r.isSome());
  EXPECT_EQ(r.unwrap(), 5);
}

TEST(OptionTest, AndThenReturnsNoneFromFn) {
  xpp::Option<int> o(4);
  auto r = o.andThen([](int) { return xpp::Option<int>(xpp::none); });
  EXPECT_TRUE(r.isNone());
}

TEST(OptionTest, AndThenPassesThroughNone) {
  xpp::Option<int> o;
  bool             called = false;
  auto             r      = o.andThen([&](int x) {
    called = true;
    return xpp::Option<int>(x);
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.isNone());
}

TEST(OptionTest, AndThenChangesType) {
  xpp::Option<int> o(3);
  auto r = o.andThen([](int x) { return xpp::Option<std::string>(std::to_string(x)); });
  static_assert(std::is_same<decltype(r), xpp::Option<std::string>>::value, "");
  EXPECT_EQ(r.unwrap(), "3");
}

/* ── orElse ───────────────────────────────────────────────────────────── */

TEST(OptionTest, OrElsePassesThroughSome) {
  xpp::Option<int> o(7);
  bool             called = false;
  auto             r      = o.orElse([&] {
    called = true;
    return xpp::Option<int>(99);
  });
  EXPECT_FALSE(called);
  EXPECT_EQ(r.unwrap(), 7);
}

TEST(OptionTest, OrElseSubstitutesOnNone) {
  xpp::Option<int> o;
  auto             r = o.orElse([] { return xpp::Option<int>(99); });
  EXPECT_EQ(r.unwrap(), 99);
}

TEST(OptionTest, OrElseFnCanReturnNone) {
  xpp::Option<int> o;
  auto             r = o.orElse([] { return xpp::Option<int>(xpp::none); });
  EXPECT_TRUE(r.isNone());
}

/* ── unwrapOrElse ─────────────────────────────────────────────────────── */

TEST(OptionTest, UnwrapOrElseReturnsValueWhenSome) {
  xpp::Option<int> o(5);
  EXPECT_EQ(std::move(o).unwrapOrElse([] { return 99; }), 5);
}

TEST(OptionTest, UnwrapOrElseCallsFnWhenNone) {
  xpp::Option<int> o;
  EXPECT_EQ(std::move(o).unwrapOrElse([] { return 99; }), 99);
}

/* ── filter ───────────────────────────────────────────────────────────── */

TEST(OptionTest, FilterKeepsValueWhenPredTrue) {
  xpp::Option<int> o(10);
  auto             r = std::move(o).filter([](int x) { return x > 5; });
  EXPECT_TRUE(r.isSome());
  EXPECT_EQ(r.unwrap(), 10);
}

TEST(OptionTest, FilterDropsValueWhenPredFalse) {
  xpp::Option<int> o(3);
  auto             r = std::move(o).filter([](int x) { return x > 5; });
  EXPECT_TRUE(r.isNone());
}

TEST(OptionTest, FilterOnNoneStaysNone) {
  xpp::Option<int> o;
  bool             called = false;
  auto             r      = std::move(o).filter([&](int) {
    called = true;
    return true;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.isNone());
}

/* ── inspect ──────────────────────────────────────────────────────────── */

TEST(OptionTest, InspectCallsFnWhenSome) {
  xpp::Option<int> o(7);
  int              seen = 0;
  o.inspect([&](const int &x) { seen = x; });
  EXPECT_EQ(seen, 7);
  EXPECT_TRUE(o.isSome());
}

TEST(OptionTest, InspectSkipsWhenNone) {
  xpp::Option<int> o;
  bool             called = false;
  o.inspect([&](const int &) { called = true; });
  EXPECT_FALSE(called);
}

TEST(OptionTest, InspectIsChainable) {
  xpp::Option<int> o(1);
  int              seen = 0;
  auto             r    = std::move(o).inspect([&](const int &x) { seen = x; }).map([](int x) {
    return x + 10;
  });
  EXPECT_EQ(seen, 1);
  EXPECT_EQ(r.unwrap(), 11);
}

/* ── okOr ─────────────────────────────────────────────────────────────── */

TEST(OptionTest, OkOrReturnsOkWhenSome) {
  xpp::Option<int>                    o(42);
  xpp::Result<int, std::string> r = std::move(o).okOr<std::string>("nope");
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.unwrap(), 42);
}

TEST(OptionTest, OkOrReturnsErrWhenNone) {
  xpp::Option<int>                    o;
  xpp::Result<int, std::string> r = std::move(o).okOr<std::string>("nope");
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.unwrapErr(), "nope");
}

/* ── okOrElse ─────────────────────────────────────────────────────────── */

TEST(OptionTest, OkOrElseReturnsOkWhenSome) {
  xpp::Option<int> o(42);
  bool             called = false;
  auto             r      = std::move(o).okOrElse([&] {
    called = true;
    return std::string("nope");
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.unwrap(), 42);
}

TEST(OptionTest, OkOrElseCallsFnWhenNone) {
  xpp::Option<int> o;
  auto             r = std::move(o).okOrElse([] { return std::string("nope"); });
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.unwrapErr(), "nope");
}
