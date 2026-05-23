/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * own_test.cpp - Tests for Own<T, Deleter>.
 *
 * Verifies Rust/std::unique_ptr-style API:
 *   - default null, ctor from raw, ctor from NonNullOwn, ctor from Option<NonNullOwn>
 *   - reset / take / release equivalence
 *   - operator* / operator-> / operator bool / nullptr comparisons
 *   - into_nonnull bridge
 *   - Tracker fixture confirms no leak / double-free
 *   - debug death tests for null deref
 *   - SFINAE: Own<void> has no operator* or operator->
 */

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include <xpp/own.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::Own<int>) == sizeof(int *),
              "Own<int> must be sizeof(int*) via niche-optimized Option<NonNullOwn> storage");

static_assert(!std::is_copy_constructible<xpp::Own<int>>::value, "Own must not be copyable");
static_assert(!std::is_copy_assignable<xpp::Own<int>>::value, "Own must not be copy-assignable");
static_assert(std::is_move_constructible<xpp::Own<int>>::value, "Own must be movable");
static_assert(std::is_move_assignable<xpp::Own<int>>::value, "Own must be move-assignable");
static_assert(std::is_default_constructible<xpp::Own<int>>::value,
              "Own must be default-constructible (yields empty)");
static_assert(std::is_nothrow_destructible<xpp::Own<int>>::value,
              "Own destructor must be noexcept");

namespace {

/* SFINAE detector: Own<void> must have no operator*. */
template <class, class = void> struct has_op_star : std::false_type {};
template <class T> struct has_op_star<T, decltype(void(*std::declval<T &>()))> : std::true_type {};

static_assert(has_op_star<xpp::Own<int>>::value, "Own<int> must have operator*");
static_assert(!has_op_star<xpp::Own<void>>::value, "Own<void> must not have operator*");

/*
 * Heap-allocated tracker (mirrors nonnull_own_test.cpp).
 */
struct Tracker {
  static int alive;
  int        value;

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

class OwnTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

/* Stateful deleter — verifies sizeof grows. */
struct StatefulDeleter {
  int *call_count;
  void operator()(Tracker *p) const noexcept {
    if (call_count) ++*call_count;
    delete p;
  }
};
static_assert(sizeof(xpp::Own<Tracker, StatefulDeleter>) > sizeof(Tracker *),
              "Own with stateful deleter must grow beyond sizeof(T*)");

} // namespace

/* ── Construction ────────────────────────────────────────────────────── */

TEST(OwnTest, DefaultConstructIsEmpty) {
  xpp::Own<int> o;
  EXPECT_FALSE(static_cast<bool>(o));
  EXPECT_EQ(o.get(), nullptr);
  EXPECT_TRUE(o == nullptr);
  EXPECT_FALSE(o != nullptr);
}

TEST(OwnTest, NullptrCtorIsEmpty) {
  xpp::Own<int> o(nullptr);
  EXPECT_FALSE(static_cast<bool>(o));
}

TEST_F(OwnTrackerTest, RawPtrCtorTakesOwnership) {
  {
    xpp::Own<Tracker> o(new Tracker(7));
    EXPECT_TRUE(static_cast<bool>(o));
    EXPECT_EQ(o->value, 7);
    EXPECT_EQ((*o).value, 7);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OwnTest, RawPtrCtorWithNullIsEmpty) {
  Tracker          *p = nullptr;
  xpp::Own<Tracker> o(p);
  EXPECT_FALSE(static_cast<bool>(o));
  EXPECT_EQ(o.get(), nullptr);
}

TEST_F(OwnTrackerTest, FromNonNullOwn) {
  {
    auto              nn = xpp::NonNullOwn<Tracker>::new_unchecked(new Tracker(3));
    xpp::Own<Tracker> o(std::move(nn));
    EXPECT_TRUE(static_cast<bool>(o));
    EXPECT_EQ(o->value, 3);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OwnTrackerTest, FromOptionNonNullOwnSome) {
  {
    auto              opt = xpp::NonNullOwn<Tracker>::from(new Tracker(4));
    xpp::Own<Tracker> o(std::move(opt));
    EXPECT_TRUE(static_cast<bool>(o));
    EXPECT_EQ(o->value, 4);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OwnTest, FromOptionNonNullOwnNoneIsEmpty) {
  xpp::Option<xpp::NonNullOwn<int>> opt;
  xpp::Own<int>                     o(std::move(opt));
  EXPECT_FALSE(static_cast<bool>(o));
}

/* ── Move semantics ──────────────────────────────────────────────────── */

TEST_F(OwnTrackerTest, MoveCtorTransfersOwnership) {
  {
    xpp::Own<Tracker> a(new Tracker(1));
    EXPECT_EQ(Tracker::alive, 1);
    xpp::Own<Tracker> b(std::move(a));
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_FALSE(static_cast<bool>(a));
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OwnTrackerTest, MoveAssignDeletesOldTarget) {
  {
    xpp::Own<Tracker> a(new Tracker(1));
    xpp::Own<Tracker> b(new Tracker(2));
    EXPECT_EQ(Tracker::alive, 2);
    b = std::move(a);
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_EQ(b->value, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OwnTrackerTest, MoveAssignFromEmpty) {
  {
    xpp::Own<Tracker> a;
    xpp::Own<Tracker> b(new Tracker(2));
    EXPECT_EQ(Tracker::alive, 1);
    b = std::move(a);
    EXPECT_EQ(Tracker::alive, 0); // old target deleted
    EXPECT_FALSE(static_cast<bool>(b));
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── nullptr assignment ──────────────────────────────────────────────── */

TEST_F(OwnTrackerTest, AssignNullptrDeletesAndClears) {
  xpp::Own<Tracker> o(new Tracker(5));
  EXPECT_EQ(Tracker::alive, 1);
  o = nullptr;
  EXPECT_FALSE(static_cast<bool>(o));
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── reset ───────────────────────────────────────────────────────────── */

TEST_F(OwnTrackerTest, ResetReplaces) {
  {
    xpp::Own<Tracker> o(new Tracker(1));
    EXPECT_EQ(Tracker::alive, 1);
    o.reset(new Tracker(2));
    EXPECT_EQ(Tracker::alive, 1); // old deleted
    EXPECT_EQ(o->value, 2);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OwnTrackerTest, ResetToNullClears) {
  xpp::Own<Tracker> o(new Tracker(7));
  EXPECT_EQ(Tracker::alive, 1);
  o.reset();
  EXPECT_FALSE(static_cast<bool>(o));
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OwnTrackerTest, ResetFromEmpty) {
  xpp::Own<Tracker> o;
  o.reset(new Tracker(9));
  EXPECT_TRUE(static_cast<bool>(o));
  EXPECT_EQ(o->value, 9);
  EXPECT_EQ(Tracker::alive, 1);
  o.reset();
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── take / release ──────────────────────────────────────────────────── */

TEST_F(OwnTrackerTest, TakeRelinquishesOwnership) {
  Tracker *raw;
  {
    xpp::Own<Tracker> o(new Tracker(3));
    raw = o.take();
    EXPECT_EQ(Tracker::alive, 1);
    EXPECT_FALSE(static_cast<bool>(o));
  }
  EXPECT_EQ(Tracker::alive, 1); // dtor did not delete
  delete raw;
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OwnTest, TakeOnEmptyReturnsNull) {
  xpp::Own<int> o;
  EXPECT_EQ(o.take(), nullptr);
}

TEST_F(OwnTrackerTest, ReleaseEquivalentToTake) {
  Tracker *raw;
  {
    xpp::Own<Tracker> o(new Tracker(4));
    raw = o.release();
    EXPECT_FALSE(static_cast<bool>(o));
  }
  delete raw;
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── operator* / operator-> debug death tests ────────────────────────── */

#if XPP_DEBUG
TEST(OwnDeathTest, DerefStarOnEmpty) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(([] {
                 xpp::Own<int> o;
                 (void)*o;
               }()),
               "Own::operator\\* on empty Own");
}

TEST(OwnDeathTest, DerefArrowOnEmpty) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  struct S {
    int v;
  };
  EXPECT_DEATH(([] {
                 xpp::Own<S> o;
                 (void)o->v;
               }()),
               "Own::operator-> on empty Own");
}
#endif

/* ── into_nonnull bridge ──────────────────────────────────────────────── */

TEST_F(OwnTrackerTest, IntoNonNullSomeWhenNonEmpty) {
  {
    xpp::Own<Tracker>                     o(new Tracker(11));
    xpp::Option<xpp::NonNullOwn<Tracker>> opt = std::move(o).into_nonnull();
    EXPECT_TRUE(opt.is_some());
    EXPECT_EQ(opt.unwrap()->value, 11);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST(OwnTest, IntoNonNullNoneWhenEmpty) {
  xpp::Own<int>                     o;
  xpp::Option<xpp::NonNullOwn<int>> opt = std::move(o).into_nonnull();
  EXPECT_TRUE(opt.is_none());
}

TEST_F(OwnTrackerTest, RoundtripThroughNonNullOwn) {
  // Own → Option<NonNullOwn> → Own: ownership preserved, no leak.
  {
    xpp::Own<Tracker> a(new Tracker(13));
    auto              opt = std::move(a).into_nonnull();
    xpp::Own<Tracker> b(std::move(opt));
    EXPECT_EQ(b->value, 13);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Stateful deleter ────────────────────────────────────────────────── */

TEST_F(OwnTrackerTest, StatefulDeleterIsInvoked) {
  int call_count = 0;
  {
    xpp::Own<Tracker, StatefulDeleter> o(new Tracker(1), StatefulDeleter{&call_count});
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OwnTrackerTest, StatefulDeleterSurvivesReset) {
  int call_count = 0;
  {
    xpp::Own<Tracker, StatefulDeleter> o(new Tracker(1), StatefulDeleter{&call_count});
    o.reset();
    EXPECT_EQ(call_count, 1);
    EXPECT_EQ(Tracker::alive, 0);
  }
}

/* ── Covariance (Wrapper<Derived> → Wrapper<Base>) ───────────────────── */

namespace {

struct Base {
  static int alive;
  int        base_value;
  Base() : base_value(0) {
    ++alive;
  }
  explicit Base(int v) : base_value(v) {
    ++alive;
  }
  Base(const Base &)            = delete;
  Base(Base &&)                 = delete;
  Base &operator=(const Base &) = delete;
  Base &operator=(Base &&)      = delete;
  virtual ~Base() {
    --alive;
  } // virtual: required for delete-through-Base*
  virtual int kind() const {
    return 1;
  }
};
int Base::alive = 0;

struct Derived : Base {
  int derived_value;
  explicit Derived(int b, int d) : Base(b), derived_value(d) {}
  int kind() const override {
    return 2;
  }
};

class CovarianceTest : public ::testing::Test {
protected:
  void SetUp() override {
    Base::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Base::alive, 0) << "leak: " << Base::alive << " Bases still alive";
  }
};

} // namespace

TEST_F(CovarianceTest, NonNullOwnDerivedToBase) {
  {
    auto                  d = xpp::NonNullOwn<Derived>::new_unchecked(new Derived(1, 2));
    xpp::NonNullOwn<Base> b(std::move(d));
    EXPECT_EQ(b->kind(), 2); // virtual dispatch → Derived
    EXPECT_EQ(b->base_value, 1);
    EXPECT_EQ(Base::alive, 1);
  }
  EXPECT_EQ(Base::alive, 0); // ~Derived runs through virtual ~Base
}

TEST_F(CovarianceTest, OptionNonNullOwnDerivedToBase) {
  {
    auto                               d_opt = xpp::NonNullOwn<Derived>::from(new Derived(3, 4));
    xpp::Option<xpp::NonNullOwn<Base>> b_opt(std::move(d_opt));
    ASSERT_TRUE(b_opt.is_some());
    EXPECT_EQ(b_opt.unwrap()->kind(), 2);
    EXPECT_EQ(Base::alive, 1);
  }
  EXPECT_EQ(Base::alive, 0);
}

TEST_F(CovarianceTest, OptionNonNullOwnFromDerivedNonNull) {
  // Construct Option<NonNullOwn<Base>> directly from NonNullOwn<Derived>.
  {
    auto d = xpp::NonNullOwn<Derived>::new_unchecked(new Derived(5, 6));
    xpp::Option<xpp::NonNullOwn<Base>> b_opt(std::move(d));
    ASSERT_TRUE(b_opt.is_some());
    EXPECT_EQ(b_opt.unwrap()->kind(), 2);
  }
  EXPECT_EQ(Base::alive, 0);
}

TEST_F(CovarianceTest, OwnDerivedToBase) {
  {
    xpp::Own<Derived> d(new Derived(7, 8));
    xpp::Own<Base>    b(std::move(d));
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b->kind(), 2);
    EXPECT_EQ(b->base_value, 7);
    EXPECT_FALSE(static_cast<bool>(d));
    EXPECT_EQ(Base::alive, 1);
  }
  EXPECT_EQ(Base::alive, 0);
}

TEST_F(CovarianceTest, OwnFromDerivedNonNullOwn) {
  // Own<Base>(NonNullOwn<Derived>&&) — covariant adoption ctor.
  {
    auto           d = xpp::NonNullOwn<Derived>::new_unchecked(new Derived(9, 10));
    xpp::Own<Base> b(std::move(d));
    EXPECT_EQ(b->kind(), 2);
  }
  EXPECT_EQ(Base::alive, 0);
}

TEST_F(CovarianceTest, OwnFromDerivedOptionNonNullOwn) {
  // Own<Base>(Option<NonNullOwn<Derived>>&&) — covariant adoption ctor.
  {
    auto           d_opt = xpp::NonNullOwn<Derived>::from(new Derived(11, 12));
    xpp::Own<Base> b(std::move(d_opt));
    EXPECT_TRUE(static_cast<bool>(b));
    EXPECT_EQ(b->kind(), 2);
  }
  EXPECT_EQ(Base::alive, 0);
}

/* Compile-time: covariance must NOT engage for unrelated types. */
namespace {
struct Unrelated {};

template <class From, class To, class = void> struct can_convert_own : std::false_type {};
template <class From, class To>
struct can_convert_own<From, To, decltype(void(xpp::Own<To>(std::declval<xpp::Own<From>>())))>
    : std::true_type {};
} // namespace

static_assert(can_convert_own<Derived, Base>::value, "Own<Derived> → Own<Base> must be allowed");
static_assert(!can_convert_own<Unrelated, Base>::value,
              "Own<Unrelated> → Own<Base> must NOT be allowed");
static_assert(!can_convert_own<Base, Derived>::value,
              "Own<Base> → Own<Derived> must NOT be allowed (only up-conversion)");
