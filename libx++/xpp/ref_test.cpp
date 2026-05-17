/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ref_test.cpp - Tests for Ref<T> and Option<Ref<T>>.
 *
 * Each refcount-changing path gets a dedicated test that asserts both
 * the visible refcount and (via the Tracker fixture) the absence of
 * leaks and double-frees.
 *
 *   - makeRef:       count = 1
 *   - copy ctor:     count += 1
 *   - copy assign:   old.count -= 1, new.count += 1
 *   - move ctor:     count unchanged, source invalid
 *   - move assign:   old.count -= 1, source invalid
 *   - .clone():      identical to copy ctor
 *   - covariant up-cast (Derived → Base): same inner, count tracked
 *   - Option<Ref<T>> Some/None transitions touch count correctly
 *   - Option<Ref<T>>::take() / unwrap(): count unchanged, Option → None
 *   - sizeof invariants
 *
 * Tracker counts live objects via a static; every TEST_F asserts the
 * count returned to zero at end of scope, so any missed dec or extra
 * delete fails the test on TearDown.
 */

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

#include <xpp/option.h>
#include <xpp/ref.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::Ref<int>) == sizeof(int *), "Ref<int> must be sizeof(int*)");
static_assert(sizeof(xpp::Option<xpp::Ref<int>>) == sizeof(int *),
              "Option<Ref<int>> niche optimisation broken");

static_assert(std::is_copy_constructible<xpp::Ref<int>>::value, "Ref must be copyable");
static_assert(std::is_copy_assignable<xpp::Ref<int>>::value, "Ref must be copy-assignable");
static_assert(std::is_move_constructible<xpp::Ref<int>>::value, "Ref must be movable");
static_assert(std::is_move_assignable<xpp::Ref<int>>::value, "Ref must be move-assignable");
static_assert(!std::is_default_constructible<xpp::Ref<int>>::value,
              "Ref must NOT be default-constructible (always non-null)");
static_assert(std::is_nothrow_destructible<xpp::Ref<int>>::value,
              "Ref destructor must be noexcept");

static_assert(std::is_default_constructible<xpp::Option<xpp::Ref<int>>>::value,
              "Option<Ref<T>> must default-construct to None");
static_assert(std::is_copy_constructible<xpp::Option<xpp::Ref<int>>>::value,
              "Option<Ref<T>> must be copyable");
static_assert(std::is_nothrow_destructible<xpp::Option<xpp::Ref<int>>>::value,
              "Option<Ref<T>> destructor must be noexcept");

namespace {

/* ── Tracker fixture ────────────────────────────────────────────────── */

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

class RefTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

/* ── makeRef: count starts at 1, single allocation ───────────────────── */

TEST_F(RefTrackerTest, MakeRefCountStartsAtOne) {
  {
    xpp::Ref<Tracker> r = xpp::makeRef<Tracker>(42);
    EXPECT_EQ(r->value, 42);
    EXPECT_EQ(r.useCount(), 1u);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RefTrackerTest, MakeRefWithDefaultCtor) {
  {
    xpp::Ref<Tracker> r = xpp::makeRef<Tracker>();
    EXPECT_EQ(r->value, 0);
    EXPECT_EQ(r.useCount(), 1u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Copy: count += 1, both Refs see the same Tracker ────────────────── */

TEST_F(RefTrackerTest, CopyCtorBumpsCount) {
  {
    xpp::Ref<Tracker> a = xpp::makeRef<Tracker>(7);
    EXPECT_EQ(a.useCount(), 1u);
    {
      xpp::Ref<Tracker> b = a; // +1
      EXPECT_EQ(a.useCount(), 2u);
      EXPECT_EQ(b.useCount(), 2u);
      EXPECT_EQ(b->value, 7);
      EXPECT_EQ(a.get(), b.get()); // same Tracker
      EXPECT_EQ(Tracker::alive, 1);
    }                              // b dies: -1
    EXPECT_EQ(a.useCount(), 1u);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RefTrackerTest, CopyAssignBumpsAndDropsCorrectly) {
  {
    xpp::Ref<Tracker> a = xpp::makeRef<Tracker>(1);
    xpp::Ref<Tracker> b = xpp::makeRef<Tracker>(2);
    EXPECT_EQ(Tracker::alive, 2);

    b = a; // b's old Tracker dropped (count was 1 → 0 → deleted)
           // a's Tracker +1
    EXPECT_EQ(Tracker::alive, 1) << "old Tracker should be deleted";
    EXPECT_EQ(a.useCount(), 2u);
    EXPECT_EQ(b->value, 1);
    EXPECT_EQ(a.get(), b.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RefTrackerTest, SelfAssignIsNoop) {
  {
    xpp::Ref<Tracker> a = xpp::makeRef<Tracker>(99);
    xpp::Ref<Tracker> &alias = a;
    alias = a; // must not blow up
    EXPECT_EQ(a.useCount(), 1u);
    EXPECT_EQ(a->value, 99);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Move: count unchanged ───────────────────────────────────────────── */

TEST_F(RefTrackerTest, MoveCtorDoesNotChangeCount) {
  {
    xpp::Ref<Tracker> a = xpp::makeRef<Tracker>(5);
    EXPECT_EQ(a.useCount(), 1u);
    xpp::Ref<Tracker> b = std::move(a);
    EXPECT_EQ(b.useCount(), 1u) << "count unchanged on move";
    EXPECT_EQ(b->value, 5);
    EXPECT_EQ(Tracker::alive, 1);
    // a is in unspecified state; do not use it
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RefTrackerTest, MoveAssignDropsOldRespectsCount) {
  {
    xpp::Ref<Tracker> a = xpp::makeRef<Tracker>(1);
    xpp::Ref<Tracker> b = xpp::makeRef<Tracker>(2);
    xpp::Ref<Tracker> c = a; // keep a alive after move
    EXPECT_EQ(a.useCount(), 2u);
    EXPECT_EQ(Tracker::alive, 2);

    b = std::move(a); // b's old (count 1) dies; a's (count 2) inherited by b
    EXPECT_EQ(Tracker::alive, 1) << "b's old Tracker should be deleted";
    EXPECT_EQ(b.useCount(), 2u);
    EXPECT_EQ(c.useCount(), 2u);
    EXPECT_EQ(b.get(), c.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── .clone(): identical to copy ctor ────────────────────────────────── */

TEST_F(RefTrackerTest, CloneEqualsCopy) {
  {
    xpp::Ref<Tracker> a = xpp::makeRef<Tracker>(11);
    xpp::Ref<Tracker> b = a.clone();
    EXPECT_EQ(a.useCount(), 2u);
    EXPECT_EQ(b.useCount(), 2u);
    EXPECT_EQ(a.get(), b.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Equality: pointer identity ──────────────────────────────────────── */

TEST_F(RefTrackerTest, EqualityIsPointerIdentity) {
  {
    xpp::Ref<Tracker> a  = xpp::makeRef<Tracker>(1);
    xpp::Ref<Tracker> b  = a;
    xpp::Ref<Tracker> c  = xpp::makeRef<Tracker>(1); // distinct Tracker
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c) << "same value, different object";
    EXPECT_TRUE(a != c);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Covariant up-cast: Ref<Derived> → Ref<Base> ─────────────────────── */

struct Base {
  static int alive;
  int        tag;

  explicit Base(int t) : tag(t) {
    ++alive;
  }
  Base(const Base &)            = delete;
  Base &operator=(const Base &) = delete;
  virtual ~Base() {
    --alive;
  }
};
int Base::alive = 0;

struct Derived : Base {
  int extra;
  explicit Derived(int t, int e) : Base(t), extra(e) {}
};

class RefCovariantTest : public ::testing::Test {
protected:
  void SetUp() override {
    Base::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Base::alive, 0) << "leak: " << Base::alive << " Base/Derived still alive";
  }
};

TEST_F(RefCovariantTest, CopyConvertDerivedToBase) {
  {
    xpp::Ref<Derived> d = xpp::makeRef<Derived>(7, 99);
    EXPECT_EQ(d.useCount(), 1u);
    EXPECT_EQ(d->tag, 7);
    EXPECT_EQ(d->extra, 99);

    xpp::Ref<Base> b = d; // covariant copy: +1
    EXPECT_EQ(d.useCount(), 2u);
    EXPECT_EQ(b.useCount(), 2u);
    EXPECT_EQ(b->tag, 7);
    EXPECT_EQ(Base::alive, 1) << "still one logical object";
  }
  EXPECT_EQ(Base::alive, 0);
}

TEST_F(RefCovariantTest, MoveConvertDerivedToBase) {
  {
    xpp::Ref<Derived> d = xpp::makeRef<Derived>(7, 99);
    xpp::Ref<Base>    b = std::move(d); // count unchanged
    EXPECT_EQ(b.useCount(), 1u);
    EXPECT_EQ(b->tag, 7);
  }
  EXPECT_EQ(Base::alive, 0);
}

/* ── Option<Ref<T>>: niche optimisation and Some/None semantics ──────── */

class OptionRefTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

TEST_F(OptionRefTrackerTest, DefaultIsNone) {
  xpp::Option<xpp::Ref<Tracker>> o;
  EXPECT_TRUE(o.isNone());
  EXPECT_FALSE(o.isSome());
  EXPECT_FALSE(static_cast<bool>(o));
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRefTrackerTest, ConstructFromRefBumpsCount) {
  {
    xpp::Ref<Tracker> r = xpp::makeRef<Tracker>(8);
    EXPECT_EQ(r.useCount(), 1u);
    {
      xpp::Option<xpp::Ref<Tracker>> o(r); // copy: +1
      EXPECT_TRUE(o.isSome());
      EXPECT_EQ(r.useCount(), 2u);
      EXPECT_EQ(Tracker::alive, 1);
    } // o dies: -1
    EXPECT_EQ(r.useCount(), 1u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRefTrackerTest, ConstructFromRefRvalueDoesNotBumpCount) {
  {
    xpp::Option<xpp::Ref<Tracker>> o(xpp::makeRef<Tracker>(8));
    EXPECT_TRUE(o.isSome());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRefTrackerTest, AssignNoneDropsRef) {
  {
    xpp::Option<xpp::Ref<Tracker>> o(xpp::makeRef<Tracker>(1));
    EXPECT_EQ(Tracker::alive, 1);
    o = xpp::none;
    EXPECT_TRUE(o.isNone());
    EXPECT_EQ(Tracker::alive, 0) << "Tracker should be deleted when Option goes None";
  }
}

TEST_F(OptionRefTrackerTest, CopyOptionBumpsCount) {
  {
    xpp::Ref<Tracker>              r = xpp::makeRef<Tracker>(3);
    xpp::Option<xpp::Ref<Tracker>> o1(r);
    xpp::Option<xpp::Ref<Tracker>> o2 = o1; // copy: +1
    EXPECT_EQ(r.useCount(), 3u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRefTrackerTest, MoveOptionDoesNotBumpCount) {
  {
    xpp::Ref<Tracker>              r = xpp::makeRef<Tracker>(3);
    xpp::Option<xpp::Ref<Tracker>> o1(r); // count = 2
    EXPECT_EQ(r.useCount(), 2u);
    xpp::Option<xpp::Ref<Tracker>> o2 = std::move(o1); // count unchanged
    EXPECT_EQ(r.useCount(), 2u);
    EXPECT_TRUE(o1.isNone());
    EXPECT_TRUE(o2.isSome());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRefTrackerTest, TakeMovesOutRefCountUnchanged) {
  {
    xpp::Ref<Tracker>              r = xpp::makeRef<Tracker>(3);
    xpp::Option<xpp::Ref<Tracker>> o(r);    // count = 2
    EXPECT_EQ(r.useCount(), 2u);

    xpp::Ref<Tracker> taken = o.take();     // moves out; count unchanged
    EXPECT_TRUE(o.isNone());
    EXPECT_EQ(r.useCount(), 2u) << "take() moves ownership, does not bump or drop";
    EXPECT_EQ(taken.get(), r.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRefTrackerTest, UnwrapOnRvalue) {
  {
    xpp::Option<xpp::Ref<Tracker>> o(xpp::makeRef<Tracker>(5)); // count = 1
    xpp::Ref<Tracker>              r = std::move(o).unwrap();
    EXPECT_EQ(r.useCount(), 1u);
    EXPECT_EQ(r->value, 5);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

} // namespace
