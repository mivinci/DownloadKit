/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * nonnull_own_test.cpp - Tests for Box<T, Deleter> and
 *                           Option<Box<T, Deleter>>.
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

#include <xpp/box.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::Box<int>) == sizeof(int *),
              "Box<int, default_delete> must be sizeof(int*) via EBO");
static_assert(sizeof(xpp::Option<xpp::Box<int>>) == sizeof(int *), "Option<Box<int>> niche broken");

static_assert(!std::is_copy_constructible<xpp::Box<int>>::value, "Box must not be copyable");
static_assert(!std::is_copy_assignable<xpp::Box<int>>::value, "Box must not be copy-assignable");
static_assert(std::is_move_constructible<xpp::Box<int>>::value, "Box must be movable");
static_assert(std::is_move_assignable<xpp::Box<int>>::value, "Box must be move-assignable");
static_assert(!std::is_default_constructible<xpp::Box<int>>::value,
              "Box must not be default-constructible");

static_assert(!std::is_copy_constructible<xpp::Option<xpp::Box<int>>>::value,
              "Option<Box<int>> must not be copyable");

namespace {

/* SFINAE detector for operator*. Used to verify Box<void> SFINAEs out. */
template <class, class = void> struct has_op_star : std::false_type {};
template <class T> struct has_op_star<T, decltype(void(*std::declval<T &>()))> : std::true_type {};

static_assert(has_op_star<xpp::Box<int>>::value, "Box<int> must have operator*");
static_assert(!has_op_star<xpp::Box<void>>::value, "Box<void> must not have operator*");

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

class BoxTrackerTest : public ::testing::Test {
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
static_assert(sizeof(xpp::Box<Tracker, CountingDeleter>) == sizeof(Tracker *),
              "Box with empty custom deleter must still be sizeof(T*)");

/*
 * A *stateful* deleter. sizeof grows by sizeof(int) (rounded for
 * alignment), so sizeof(Box) > sizeof(T*).
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
static_assert(sizeof(xpp::Box<Tracker, StatefulDeleter>) > sizeof(Tracker *),
              "Box with stateful deleter must grow beyond sizeof(T*)");

} // namespace

/* ── Box construction ──────────────────────────────────────── */

TEST_F(BoxTrackerTest, NewUncheckedHappyPath) {
  {
    auto u = xpp::Box<Tracker>::from_raw(new Tracker(7));
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(u->value, 7);
    EXPECT_EQ((*u).value, 7);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

#if XPP_DEBUG
TEST(BoxDeathTest, NewUncheckedOnNullDebug) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] { (void)xpp::Box<int>::from_raw(nullptr); }()),
               "Box::from_raw: pointer is null");
}
#endif

TEST_F(BoxTrackerTest, FromNonNullReturnsSome) {
  {
    auto opt = xpp::Box<Tracker>::try_from_raw(new Tracker(11));
    ASSERT_TRUE(opt.is_some());
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(opt.unwrap()->value, 11); // unwrap() const& returns Tracker*
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(BoxTest, FromNullptrReturnsNone) {
  Tracker *p   = nullptr;
  auto     opt = xpp::Box<Tracker>::try_from_raw(p);
  EXPECT_TRUE(opt.is_none());
}

/* ── Move semantics ──────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, MoveCtorTransfersOwnershipNoDoubleFree) {
  {
    auto a = xpp::Box<Tracker>::from_raw(new Tracker(1));
    EXPECT_EQ(Tracker::alive, 1);
    auto b = std::move(a);
    EXPECT_EQ(Tracker::alive, 1); // still 1, not 0 (no premature delete)
    EXPECT_EQ(b->value, 1);
  }
  EXPECT_EQ(Tracker::alive, 0); // single delete on b's destruction
}

TEST_F(BoxTrackerTest, MoveAssignReplacesOldTarget) {
  {
    auto a = xpp::Box<Tracker>::from_raw(new Tracker(1));
    auto b = xpp::Box<Tracker>::from_raw(new Tracker(2));
    EXPECT_EQ(Tracker::alive, 2);
    b = std::move(a);
    EXPECT_EQ(Tracker::alive, 1); // old target of b deleted
    EXPECT_EQ(b->value, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── release / as_nonnull ─────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, ReleaseRelinquishesOwnership) {
  Tracker *raw;
  {
    auto u = xpp::Box<Tracker>::from_raw(new Tracker(3));
    raw    = std::move(u).into_raw();
    // u is no longer usable; raw owns the object now.
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 1); // release prevented dtor delete
  delete raw;
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, AsNonNullBorrows) {
  auto u    = xpp::Box<Tracker>::from_raw(new Tracker(9));
  auto view = u.as_nonnull();
  EXPECT_EQ(view.get(), u.get());
  EXPECT_EQ((*view).value, 9);
  // u still owns; view is non-owning.
}

/* ── Option construction ─────────────────────────────────────────────── */

TEST(OptionBoxTest, DefaultIsNone) {
  xpp::Option<xpp::Box<int>> o;
  EXPECT_TRUE(o.is_none());
  EXPECT_FALSE(static_cast<bool>(o));
}

TEST(OptionBoxTest, NoneTagIsNone) {
  xpp::Option<xpp::Box<int>> o(xpp::none);
  EXPECT_TRUE(o.is_none());
}

TEST_F(BoxTrackerTest, FromCtorAndDestructorFreesMemory) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(4)));
    EXPECT_TRUE(o.is_some());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, OptionMoveCtorTransfersOwnership) {
  {
    xpp::Option<xpp::Box<Tracker>> a(xpp::Box<Tracker>::from_raw(new Tracker(5)));
    xpp::Option<xpp::Box<Tracker>> b(std::move(a));
    EXPECT_TRUE(b.is_some());
    EXPECT_TRUE(a.is_none());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, AssignNoneClearsAndFrees) {
  xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(6)));
  EXPECT_EQ(Tracker::alive, 1);
  o = xpp::none;
  EXPECT_TRUE(o.is_none());
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── unwrap (asymmetric) ─────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, UnwrapConstRefBorrows) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(8)));
    const auto                    &cref = o;
    Tracker                       *raw  = cref.unwrap();
    static_assert(std::is_same<decltype(cref.unwrap()), Tracker *>::value,
                  "const& unwrap must return T*");
    EXPECT_EQ(raw->value, 8);
    EXPECT_TRUE(o.is_some()); // borrow does not consume
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, UnwrapRvalueConsumes) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(9)));
    auto                           owned = std::move(o).unwrap();
    static_assert(std::is_same<decltype(std::move(o).unwrap()), xpp::Box<Tracker>>::value,
                  "&& unwrap must return Box");
    EXPECT_EQ(owned->value, 9);
    EXPECT_TRUE(o.is_none()); // consumed
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionBoxDeathTest, UnwrapOnNoneAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Option<xpp::Box<int>> o;
                 (void)o.unwrap();
               }()),
               "unwrap\\(\\) on None Option");
}

/* ── expect ──────────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, ExpectHappyPathConsumes) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(10)));
    auto                           owned = std::move(o).expect("must be set");
    EXPECT_EQ(owned->value, 10);
    EXPECT_TRUE(o.is_none());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionBoxDeathTest, ExpectOnNoneAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Option<xpp::Box<int>> o;
                 (void)std::move(o).expect("missing!");
               }()),
               "missing!");
}

/* ── unwrap_or / take ─────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, UnwrapOrReturnsValueWhenSome) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(1)));
    auto                           fb    = xpp::Box<Tracker>::from_raw(new Tracker(99));
    auto                           owned = std::move(o).unwrap_or(std::move(fb));
    EXPECT_EQ(owned->value, 1);
    // fb's Tracker(99) was deleted when fb went out of scope (not used).
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, UnwrapOrReturnsFallbackWhenNone) {
  {
    xpp::Option<xpp::Box<Tracker>> o;
    auto                           fb    = xpp::Box<Tracker>::from_raw(new Tracker(99));
    auto                           owned = std::move(o).unwrap_or(std::move(fb));
    EXPECT_EQ(owned->value, 99);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, TakeReturnsSomeAndClears) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(2)));
    auto                           taken = o.take();
    EXPECT_TRUE(taken.is_some());
    EXPECT_TRUE(o.is_none());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── map ─────────────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, MapConstRefViewDoesNotConsume) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(4)));
    auto                           r = o.map([](xpp::NonNull<Tracker> p) { return p->value * 2; });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap(), 8);
    EXPECT_TRUE(o.is_some()); // const& map does not consume
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, MapRvalueConsumes) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(5)));
    auto r = std::move(o).map([](xpp::Box<Tracker> &&p) { return std::to_string(p->value); });
    static_assert(std::is_same<decltype(r), xpp::Option<std::string>>::value, "");
    EXPECT_EQ(r.unwrap(), "5");
    EXPECT_TRUE(o.is_none());
    // Tracker was deleted when the lambda's parameter went out of scope.
    EXPECT_EQ(Tracker::alive, 0);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, MapPassesThroughNone) {
  xpp::Option<xpp::Box<Tracker>> o;
  bool                           called = false;
  auto                           r      = o.map([&](xpp::NonNull<Tracker>) {
    called = true;
    return 0;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── and_then ─────────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, AndThenChainsAndReturnsOption) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(6)));
    auto                           r = std::move(o).and_then([](xpp::Box<Tracker> &&p) {
      // Re-wrap as Option<Box<Tracker>> if value is positive.
      if (p->value > 0) return xpp::Option<xpp::Box<Tracker>>(std::move(p));
      return xpp::Option<xpp::Box<Tracker>>(xpp::none);
    });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap()->value, 6);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, AndThenReturnsNoneFromFn) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(6)));
    auto                           r = std::move(o).and_then([](xpp::Box<Tracker> &&) {
      // Drop the input; return None.
      return xpp::Option<int>(xpp::none);
    });
    EXPECT_TRUE(r.is_none());
    // p was deleted inside the lambda.
    EXPECT_EQ(Tracker::alive, 0);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionBoxTest, AndThenPassesThroughNone) {
  xpp::Option<xpp::Box<int>> o;
  bool                       called = false;
  auto                       r      = std::move(o).and_then([&](xpp::Box<int> &&) {
    called = true;
    return xpp::Option<int>(0);
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── or_else ──────────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, OrElsePassesThroughSome) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(7)));
    bool                           called = false;
    auto                           r      = std::move(o).or_else([&] {
      called = true;
      return xpp::Option<xpp::Box<Tracker>>(xpp::none);
    });
    EXPECT_FALSE(called);
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap()->value, 7);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, OrElseSubstitutesOnNone) {
  {
    xpp::Option<xpp::Box<Tracker>> o;
    auto                           r = std::move(o).or_else(
      [] { return xpp::Option<xpp::Box<Tracker>>(xpp::Box<Tracker>::from_raw(new Tracker(8))); });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(r.unwrap()->value, 8);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── unwrap_or_else ────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, UnwrapOrElseReturnsValueWhenSome) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(11)));
    bool                           called = false;
    auto                           p      = std::move(o).unwrap_or_else([&] {
      called = true;
      return xpp::Box<Tracker>::from_raw(new Tracker(0));
    });
    EXPECT_FALSE(called);
    EXPECT_EQ(p->value, 11);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, UnwrapOrElseCallsFnWhenNone) {
  {
    xpp::Option<xpp::Box<Tracker>> o;
    auto                           p =
      std::move(o).unwrap_or_else([] { return xpp::Box<Tracker>::from_raw(new Tracker(99)); });
    EXPECT_EQ(p->value, 99);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── filter ──────────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, FilterKeepsWhenPredTrue) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(10)));
    auto r = std::move(o).filter([](xpp::NonNull<Tracker> p) { return p->value > 5; });
    EXPECT_TRUE(r.is_some());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, FilterDropsAndDeletesWhenPredFalse) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(3)));
    auto r = std::move(o).filter([](xpp::NonNull<Tracker> p) { return p->value > 5; });
    EXPECT_TRUE(r.is_none());
    // Object must have been deleted by filter.
    EXPECT_EQ(Tracker::alive, 0);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionBoxTest, FilterOnNoneStaysNone) {
  xpp::Option<xpp::Box<int>> o;
  bool                       called = false;
  auto                       r      = std::move(o).filter([&](xpp::NonNull<int>) {
    called = true;
    return true;
  });
  EXPECT_FALSE(called);
  EXPECT_TRUE(r.is_none());
}

/* ── inspect ─────────────────────────────────────────────────────────── */

TEST_F(BoxTrackerTest, InspectCallsFnWhenSome) {
  {
    xpp::Option<xpp::Box<Tracker>> o(xpp::Box<Tracker>::from_raw(new Tracker(7)));
    int                            seen = 0;
    o.inspect([&](xpp::NonNull<Tracker> p) { seen = p->value; });
    EXPECT_EQ(seen, 7);
    EXPECT_TRUE(o.is_some()); // const& inspect does not consume
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OptionBoxTest, InspectSkipsWhenNone) {
  xpp::Option<xpp::Box<int>> o;
  bool                       called = false;
  o.inspect([&](xpp::NonNull<int>) { called = true; });
  EXPECT_FALSE(called);
}

/* ── Custom (empty) deleter — still EBO ──────────────────────────────── */

TEST_F(BoxTrackerTest, CustomEmptyDeleterIsInvoked) {
  CountingDeleter::calls = 0;
  {
    auto u = xpp::Box<Tracker, CountingDeleter>::from_raw(new Tracker(1));
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(CountingDeleter::calls, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, CustomEmptyDeleterInOptionIsInvoked) {
  CountingDeleter::calls = 0;
  {
    xpp::Option<xpp::Box<Tracker, CountingDeleter>> o(
      xpp::Box<Tracker, CountingDeleter>::from_raw(new Tracker(2)));
    EXPECT_TRUE(o.is_some());
  }
  EXPECT_EQ(CountingDeleter::calls, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Stateful deleter — sizeof grows ─────────────────────────────────── */

TEST_F(BoxTrackerTest, StatefulDeleterCarriesState) {
  int call_count = 0;
  {
    auto u =
      xpp::Box<Tracker, StatefulDeleter>::from_raw(new Tracker(3), StatefulDeleter{&call_count});
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(BoxTrackerTest, StatefulDeleterAccessibleViaGetDeleter) {
  int  call_count = 0;
  auto u =
    xpp::Box<Tracker, StatefulDeleter>::from_raw(new Tracker(4), StatefulDeleter{&call_count});
  EXPECT_EQ(u.get_deleter().call_count, &call_count);
}
