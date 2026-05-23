/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nonnull_own_test.cpp - Tests for NonNullOwn<T, Deleter> and
 *                           Option<NonNullOwn<T, Deleter>>.
 *
 * Uses a Tracker fixture (heap-allocated) to verify:
 *   - no leaks
 *   - no double-frees
 *   - correct deleter invocation count
 *
 * Also exercises:
 *   - default_delete (empty → EBO, sizeof == sizeof(T*))
 *   - a custom *empty* deleter (CountingDeleter — still EBO)
 *   - a *stateful* deleter (StatefulDeleter — sizeof grows)
 *   - asymmetric unwrap / combinator semantics (const& borrow vs && consume)
 *   - SFINAE-removed operator* / operator-> for T = void
 *   - copy-construction is deleted (compile-time)
 */

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

#include <xpp/nonnull_own.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::NonNullOwn<int>) == sizeof(int *),
              "NonNullOwn<int, default_delete> must be sizeof(int*) via EBO");
static_assert(sizeof(xpp::Option<xpp::NonNullOwn<int>>) == sizeof(int *),
              "Option<NonNullOwn<int>> niche broken");

static_assert(!std::is_copy_constructible<xpp::NonNullOwn<int>>::value,
              "NonNullOwn must not be copyable");
static_assert(!std::is_copy_assignable<xpp::NonNullOwn<int>>::value,
              "NonNullOwn must not be copy-assignable");
static_assert(std::is_move_constructible<xpp::NonNullOwn<int>>::value,
              "NonNullOwn must be movable");
static_assert(std::is_move_assignable<xpp::NonNullOwn<int>>::value,
              "NonNullOwn must be move-assignable");
static_assert(!std::is_default_constructible<xpp::NonNullOwn<int>>::value,
              "NonNullOwn must not be default-constructible");

static_assert(!std::is_copy_constructible<xpp::Option<xpp::NonNullOwn<int>>>::value,
              "Option<NonNullOwn<int>> must not be copyable");

namespace {

/* SFINAE detector for operator*. Used to verify NonNullOwn<void> SFINAEs out. */
template <class, class = void> struct has_op_star : std::false_type {};
template <class T> struct has_op_star<T, decltype(void(*std::declval<T &>()))> : std::true_type {};

static_assert(has_op_star<xpp::NonNullOwn<int>>::value, "NonNullOwn<int> must have operator*");
static_assert(!has_op_star<xpp::NonNullOwn<void>>::value,
              "NonNullOwn<void> must not have operator*");

/*
 * Heap-allocated tracker. Counts ctor/dtor calls so we can prove no
 * leaks and no double-frees.
 */
struct Tracker {
  static int alive;

  int value;

  Tracker() : value(0) {
    ++alive;
  }
  explicit Tracker(int v) : value(v) {
    ++alive;
  }
  Tracker(const Tracker &)            = delete;
  Tracker(Tracker &&)                 = delete;
  Tracker &operator=(const Tracker &) = delete;
  Tracker &operator=(Tracker &&)      = delete;

  ~Tracker() {
    --alive;
  }
};
int Tracker::alive = 0;

class NonNullOwnTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

/*
 * An *empty* custom deleter. Records calls in a static counter. Must be
 * empty (no data members) so EBO still applies → sizeof unchanged.
 */
struct CountingDeleter {
  static int calls;
  void       operator()(Tracker *p) const noexcept {
    ++calls;
    delete p;
  }
};
int CountingDeleter::calls = 0;

static_assert(std::is_empty<CountingDeleter>::value, "CountingDeleter must be empty for EBO");
static_assert(sizeof(xpp::NonNullOwn<Tracker, CountingDeleter>) == sizeof(Tracker *),
              "NonNullOwn with empty custom deleter must still be sizeof(T*)");

/*
 * A *stateful* deleter. sizeof grows by sizeof(int) (rounded for
 * alignment), so sizeof(NonNullOwn) > sizeof(T*).
 */
struct StatefulDeleter {
  int *call_count;
  void operator()(Tracker *p) const noexcept {
    if (call_count) ++*call_count;
    delete p;
  }
};
static_assert(!std::is_empty<StatefulDeleter>::value,
              "StatefulDeleter must be non-empty for this test");
static_assert(sizeof(xpp::NonNullOwn<Tracker, StatefulDeleter>) > sizeof(Tracker *),
              "NonNullOwn with stateful deleter must grow beyond sizeof(T*)");

} // namespace

/* ── NonNullOwn construction ──────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, NewUncheckedHappyPath) {
  {
    auto u = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(7));
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(u->value, 7);
    EXPECT_EQ((*u).value, 7);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

#if XPP_DEBUG
TEST(NonNullOwnDeathTest, NewUncheckedOnNullDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] { (void)xpp::NonNullOwn<int>::new_unchecked(nullptr); }()),
               "NonNullOwn::new_unchecked: pointer is null");
}
#endif

TEST_F(NonNullOwnTrackerTest, FromNonNullReturnsSome) {
  {
    auto opt = xpp::NonNullOwn<Tracker>::from(new Tracker(11));
    ASSERT_TRUE(opt.is_some());
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(opt.unwrap()->value, 11); // unwrap() const& returns Tracker*
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(NonNullOwnTest, FromNullptrReturnsNone) {
  Tracker *p   = nullptr;
  auto     opt = xpp::NonNullOwn<Tracker>::from(p);
  EXPECT_TRUE(opt.is_none());
}

/* ── Move semantics ──────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, MoveCtorTransfersOwnershipNoDoubleFree) {
  {
    auto a = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(1));
    EXPECT_EQ(Tracker::alive, 1);
    auto b = std::move(a);
    EXPECT_EQ(Tracker::alive, 1); // still 1, not 0 (no premature delete)
    EXPECT_EQ(b->value, 1);
  }
  EXPECT_EQ(Tracker::alive, 0); // single delete on b's destruction
}

TEST_F(NonNullOwnTrackerTest, MoveAssignReplacesOldTarget) {
  {
    auto a = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(1));
    auto b = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(2));
    EXPECT_EQ(Tracker::alive, 2);
    b = std::move(a);
    EXPECT_EQ(Tracker::alive, 1); // old target of b deleted
    EXPECT_EQ(b->value, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── release / as_nonnull ─────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, ReleaseRelinquishesOwnership) {
  Tracker *raw;
  {
    auto u = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(3));
    raw    = std::move(u).release();
    // u is no longer usable; raw owns the object now.
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 1); // release prevented dtor delete
  delete raw;
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, AsNonNullBorrows) {
  auto u    = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(9));
  auto view = u.as_nonnull();
  EXPECT_EQ(view.get(), u.get());
  EXPECT_EQ((*view).value, 9);
  // u still owns; view is non-owning.
}

/* ── Option construction ─────────────────────────────────────────────── */

TEST(OptionNonNullOwnTest, DefaultIsNone) {
  xpp::Option<xpp::NonNullOwn<int>> o;
  EXPECT_TRUE(o.is_none());
  EXPECT_FALSE(static_cast<bool>(o));
}

TEST(OptionNonNullOwnTest, NoneTagIsNone) {
  xpp::Option<xpp::NonNullOwn<int>> o(xpp::none);
  EXPECT_TRUE(o.is_none());
}

TEST_F(NonNullOwnTrackerTest, FromCtorAndDestructorFreesMemory) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(4)));
    EXPECT_TRUE(o.is_some());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, OptionMoveCtorTransfersOwnership) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> a(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(5)));
    xpp::Option<xpp::NonNullOwn<Tracker>> b(std::move(a));
    EXPECT_TRUE(b.is_some());
    EXPECT_TRUE(a.is_none());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, AssignNoneClearsAndFrees) {
  xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(6)));
  EXPECT_EQ(Tracker::alive, 1);
  o = xpp::none;
  EXPECT_TRUE(o.is_none());
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── unwrap (asymmetric) ─────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, UnwrapConstRefBorrows) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(8)));
    const auto                           &cref = o;
    Tracker                              *raw  = cref.unwrap();
    static_assert(std::is_same<decltype(cref.unwrap()), Tracker *>::value,
                  "const& unwrap must return T*");
    EXPECT_EQ(raw->value, 8);
    EXPECT_TRUE(o.is_some()); // borrow does not consume
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, UnwrapRvalueConsumes) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(9)));
    auto                                  owned = std::move(o).unwrap();
    static_assert(std::is_same<decltype(std::move(o).unwrap()), xpp::NonNullOwn<Tracker>>::value,
                  "&& unwrap must return NonNullOwn");
    EXPECT_EQ(owned->value, 9);
    EXPECT_TRUE(o.is_none()); // consumed
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionNonNullOwnDeathTest, UnwrapOnNoneAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Option<xpp::NonNullOwn<int>> o;
                 (void)o.unwrap();
               }()),
               "unwrap\\(\\) on None Option");
}

/* ── expect ──────────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, ExpectHappyPathConsumes) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(
      xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(10)));
    auto owned = std::move(o).expect("must be set");
    EXPECT_EQ(owned->value, 10);
    EXPECT_TRUE(o.is_none());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionNonNullOwnDeathTest, ExpectOnNoneAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Option<xpp::NonNullOwn<int>> o;
                 (void)std::move(o).expect("missing!");
               }()),
               "missing!");
}

/* ── unwrap_or / take ─────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, UnwrapOrReturnsValueWhenSome) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(1)));
    auto fb    = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(99));
    auto owned = std::move(o).unwrap_or(std::move(fb));
    EXPECT_EQ(owned->value, 1);
    // fb's Tracker(99) was deleted when fb went out of scope (not used).
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, UnwrapOrReturnsFallbackWhenNone) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o;
    auto fb    = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(99));
    auto owned = std::move(o).unwrap_or(std::move(fb));
    EXPECT_EQ(owned->value, 99);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, TakeReturnsSomeAndClears) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(2)));
    auto                                  taken = o.take();
    EXPECT_TRUE(taken.is_some());
    EXPECT_TRUE(o.is_none());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── map ─────────────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, MapConstRefViewDoesNotConsume) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(4)));
    auto r = o.map([](xpp::NonNull<Tracker> p) { return p->value * 2; });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap(), 8);
    EXPECT_TRUE(o.is_some()); // const& map does not consume
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, MapRvalueConsumes) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(5)));
    auto                                  r =
      std::move(o).map([](xpp::NonNullOwn<Tracker> &&p) { return std::to_string(p->value); });
    static_assert(std::is_same<decltype(r), xpp::Option<std::string>>::value, "");
    EXPECT_EQ(r.unwrap(), "5");
    EXPECT_TRUE(o.is_none());
    // Tracker was deleted when the lambda's parameter went out of scope.
    EXPECT_EQ(Tracker::alive, 0);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, MapPassesThroughNone) {
  xpp::Option<xpp::NonNullOwn<Tracker>> o;
  bool                                  called = false;
  auto                                  r      = o.map([&](xpp::NonNull<Tracker>) {
    called = true;
    return 0;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── and_then ─────────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, AndThenChainsAndReturnsOption) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(6)));
    auto r = std::move(o).and_then([](xpp::NonNullOwn<Tracker> &&p) {
      // Re-wrap as Option<NonNullOwn<Tracker>> if value is positive.
      if (p->value > 0) return xpp::Option<xpp::NonNullOwn<Tracker>>(std::move(p));
      return xpp::Option<xpp::NonNullOwn<Tracker>>(xpp::none);
    });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap()->value, 6);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, AndThenReturnsNoneFromFn) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(6)));
    auto                                  r = std::move(o).and_then([](xpp::NonNullOwn<Tracker> &&) {
      // Drop the input; return None.
      return xpp::Option<int>(xpp::none);
    });
    EXPECT_TRUE(r.is_none());
    // p was deleted inside the lambda.
    EXPECT_EQ(Tracker::alive, 0);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionNonNullOwnTest, AndThenPassesThroughNone) {
  xpp::Option<xpp::NonNullOwn<int>> o;
  bool                              called = false;
  auto                              r      = std::move(o).and_then([&](xpp::NonNullOwn<int> &&) {
    called = true;
    return xpp::Option<int>(0);
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── or_else ──────────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, OrElsePassesThroughSome) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(7)));
    bool                                  called = false;
    auto                                  r      = std::move(o).or_else([&] {
      called = true;
      return xpp::Option<xpp::NonNullOwn<Tracker>>(xpp::none);
    });
    EXPECT_FALSE(called);
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap()->value, 7);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, OrElseSubstitutesOnNone) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o;
    auto                                  r = std::move(o).or_else([] {
      return xpp::Option<xpp::NonNullOwn<Tracker>>(
        xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(8)));
    });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap()->value, 8);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── unwrap_or_else ────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, UnwrapOrElseReturnsValueWhenSome) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(
      xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(11)));
    bool called = false;
    auto p      = std::move(o).unwrap_or_else([&] {
      called = true;
      return xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(0));
    });
    EXPECT_FALSE(called);
    EXPECT_EQ(p->value, 11);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, UnwrapOrElseCallsFnWhenNone) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o;
    auto                                  p = std::move(o).unwrap_or_else(
      [] { return xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(99)); });
    EXPECT_EQ(p->value, 99);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── filter ──────────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, FilterKeepsWhenPredTrue) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(
      xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(10)));
    auto r = std::move(o).filter([](xpp::NonNull<Tracker> p) { return p->value > 5; });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, FilterDropsAndDeletesWhenPredFalse) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(3)));
    auto r = std::move(o).filter([](xpp::NonNull<Tracker> p) { return p->value > 5; });
    EXPECT_TRUE(r.is_none());
    // Object must have been deleted by filter.
    EXPECT_EQ(Tracker::alive, 0);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionNonNullOwnTest, FilterOnNoneStaysNone) {
  xpp::Option<xpp::NonNullOwn<int>> o;
  bool                              called = false;
  auto                              r      = std::move(o).filter([&](xpp::NonNull<int>) {
    called = true;
    return true;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── inspect ─────────────────────────────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, InspectCallsFnWhenSome) {
  {
    xpp::Option<xpp::NonNullOwn<Tracker>> o(xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(7)));
    int                                   seen = 0;
    o.inspect([&](xpp::NonNull<Tracker> p) { seen = p->value; });
    EXPECT_EQ(seen, 7);
    EXPECT_TRUE(o.is_some()); // const& inspect does not consume
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionNonNullOwnTest, InspectSkipsWhenNone) {
  xpp::Option<xpp::NonNullOwn<int>> o;
  bool                              called = false;
  o.inspect([&](xpp::NonNull<int>) { called = true; });
  EXPECT_FALSE(called);
}

/* ── Custom (empty) deleter — still EBO ──────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, CustomEmptyDeleterIsInvoked) {
  CountingDeleter::calls = 0;
  {
    auto u = xpp::NonNullOwn<Tracker, CountingDeleter>::new_unchecked(new Tracker(1));
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(CountingDeleter::calls, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, CustomEmptyDeleterInOptionIsInvoked) {
  CountingDeleter::calls = 0;
  {
    xpp::Option<xpp::NonNullOwn<Tracker, CountingDeleter>> o(
      xpp::NonNullOwn<Tracker, CountingDeleter>::new_unchecked(new Tracker(2)));
    EXPECT_TRUE(o.is_some());
  }
  EXPECT_EQ(CountingDeleter::calls, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Stateful deleter — sizeof grows ─────────────────────────────────── */

TEST_F(NonNullOwnTrackerTest, StatefulDeleterCarriesState) {
  int call_count = 0;
  {
    auto u = xpp::NonNullOwn<Tracker, StatefulDeleter>::new_unchecked(new Tracker(3),
                                                                     StatefulDeleter{&call_count});
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(NonNullOwnTrackerTest, StatefulDeleterAccessibleViaGetDeleter) {
  int  call_count = 0;
  auto u          = xpp::NonNullOwn<Tracker, StatefulDeleter>::new_unchecked(new Tracker(4),
                                                                            StatefulDeleter{&call_count});
  EXPECT_EQ(u.get_deleter().call_count, &call_count);
}
