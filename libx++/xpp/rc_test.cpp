/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * rc_test.cpp - Tests for Rc<T> and Option<Rc<T>>.
 *
 * Each refcount-changing path gets a dedicated test that asserts both
 * the visible refcount and (via the Tracker fixture) the absence of
 * leaks and double-frees.
 *
 *   - makeRc:        count = 1
 *   - copy ctor:     count += 1
 *   - copy assign:   old.count -= 1, new.count += 1
 *   - move ctor:     count unchanged, source invalid
 *   - move assign:   old.count -= 1, source invalid
 *   - .clone() / Rc<T>::clone(&r): identical to copy ctor
 *   - covariant up-cast (Derived → Base): same inner, count tracked
 *   - Option<Rc<T>> Some/None transitions touch count correctly
 *   - Option<Rc<T>>::take() / unwrap(): count unchanged, Option → None
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
#include <xpp/rc.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::Rc<int>) == sizeof(int *), "Rc<int> must be sizeof(int*)");
static_assert(sizeof(xpp::Option<xpp::Rc<int>>) == sizeof(int *),
              "Option<Rc<int>> niche optimisation broken");

static_assert(std::is_copy_constructible<xpp::Rc<int>>::value, "Rc must be copyable");
static_assert(std::is_copy_assignable<xpp::Rc<int>>::value, "Rc must be copy-assignable");
static_assert(std::is_move_constructible<xpp::Rc<int>>::value, "Rc must be movable");
static_assert(std::is_move_assignable<xpp::Rc<int>>::value, "Rc must be move-assignable");
static_assert(!std::is_default_constructible<xpp::Rc<int>>::value,
              "Rc must NOT be default-constructible (always non-null)");
static_assert(std::is_nothrow_destructible<xpp::Rc<int>>::value,
              "Rc destructor must be noexcept");

static_assert(std::is_default_constructible<xpp::Option<xpp::Rc<int>>>::value,
              "Option<Rc<T>> must default-construct to None");
static_assert(std::is_copy_constructible<xpp::Option<xpp::Rc<int>>>::value,
              "Option<Rc<T>> must be copyable");
static_assert(std::is_nothrow_destructible<xpp::Option<xpp::Rc<int>>>::value,
              "Option<Rc<T>> destructor must be noexcept");

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

class RcTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

/* ── makeRc: count starts at 1, single allocation ────────────────────── */

TEST_F(RcTrackerTest, MakeRcCountStartsAtOne) {
  {
    xpp::Rc<Tracker> r = xpp::makeRc<Tracker>(42);
    EXPECT_EQ(r->value, 42);
    EXPECT_EQ(r.strongCount(), 1u);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RcTrackerTest, MakeRcWithDefaultCtor) {
  {
    xpp::Rc<Tracker> r = xpp::makeRc<Tracker>();
    EXPECT_EQ(r->value, 0);
    EXPECT_EQ(r.strongCount(), 1u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Copy: count += 1, both Rcs see the same Tracker ─────────────────── */

TEST_F(RcTrackerTest, CopyCtorBumpsCount) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(7);
    EXPECT_EQ(a.strongCount(), 1u);
    {
      xpp::Rc<Tracker> b = a; // +1
      EXPECT_EQ(a.strongCount(), 2u);
      EXPECT_EQ(b.strongCount(), 2u);
      EXPECT_EQ(b->value, 7);
      EXPECT_EQ(a.get(), b.get()); // same Tracker
      EXPECT_EQ(Tracker::alive, 1);
    }                              // b dies: -1
    EXPECT_EQ(a.strongCount(), 1u);
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RcTrackerTest, CopyAssignBumpsAndDropsCorrectly) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(1);
    xpp::Rc<Tracker> b = xpp::makeRc<Tracker>(2);
    EXPECT_EQ(Tracker::alive, 2);

    b = a; // b's old Tracker dropped (count was 1 → 0 → deleted)
           // a's Tracker +1
    EXPECT_EQ(Tracker::alive, 1) << "old Tracker should be deleted";
    EXPECT_EQ(a.strongCount(), 2u);
    EXPECT_EQ(b->value, 1);
    EXPECT_EQ(a.get(), b.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RcTrackerTest, SelfAssignIsNoop) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(99);
    xpp::Rc<Tracker> &alias = a;
    alias = a; // must not blow up
    EXPECT_EQ(a.strongCount(), 1u);
    EXPECT_EQ(a->value, 99);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Move: count unchanged ───────────────────────────────────────────── */

TEST_F(RcTrackerTest, MoveCtorDoesNotChangeCount) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(5);
    EXPECT_EQ(a.strongCount(), 1u);
    xpp::Rc<Tracker> b = std::move(a);
    EXPECT_EQ(b.strongCount(), 1u) << "count unchanged on move";
    EXPECT_EQ(b->value, 5);
    EXPECT_EQ(Tracker::alive, 1);
    // a is in unspecified state; do not use it
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RcTrackerTest, MoveAssignDropsOldRespectsCount) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(1);
    xpp::Rc<Tracker> b = xpp::makeRc<Tracker>(2);
    xpp::Rc<Tracker> c = a; // keep a alive after move
    EXPECT_EQ(a.strongCount(), 2u);
    EXPECT_EQ(Tracker::alive, 2);

    b = std::move(a); // b's old (count 1) dies; a's (count 2) inherited by b
    EXPECT_EQ(Tracker::alive, 1) << "b's old Tracker should be deleted";
    EXPECT_EQ(b.strongCount(), 2u);
    EXPECT_EQ(c.strongCount(), 2u);
    EXPECT_EQ(b.get(), c.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── .clone() and Rc<T>::clone(&r): identical to copy ctor ───────────── */

TEST_F(RcTrackerTest, CloneEqualsCopy) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(11);
    xpp::Rc<Tracker> b = a.clone();
    EXPECT_EQ(a.strongCount(), 2u);
    EXPECT_EQ(b.strongCount(), 2u);
    EXPECT_EQ(a.get(), b.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(RcTrackerTest, StaticCloneEqualsMemberClone) {
  {
    xpp::Rc<Tracker> a = xpp::makeRc<Tracker>(13);
    xpp::Rc<Tracker> b = xpp::Rc<Tracker>::clone(a); // Rust-style spelling
    EXPECT_EQ(a.strongCount(), 2u);
    EXPECT_EQ(b.strongCount(), 2u);
    EXPECT_EQ(a.get(), b.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Equality: pointer identity ──────────────────────────────────────── */

TEST_F(RcTrackerTest, EqualityIsPointerIdentity) {
  {
    xpp::Rc<Tracker> a  = xpp::makeRc<Tracker>(1);
    xpp::Rc<Tracker> b  = a;
    xpp::Rc<Tracker> c  = xpp::makeRc<Tracker>(1); // distinct Tracker
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c) << "same value, different object";
    EXPECT_TRUE(a != c);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

/* ── Covariant up-cast: Rc<Derived> → Rc<Base> ──────────────────────── */

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

class RcCovariantTest : public ::testing::Test {
protected:
  void SetUp() override {
    Base::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Base::alive, 0) << "leak: " << Base::alive << " Base/Derived still alive";
  }
};

TEST_F(RcCovariantTest, CopyConvertDerivedToBase) {
  {
    xpp::Rc<Derived> d = xpp::makeRc<Derived>(7, 99);
    EXPECT_EQ(d.strongCount(), 1u);
    EXPECT_EQ(d->tag, 7);
    EXPECT_EQ(d->extra, 99);

    xpp::Rc<Base> b = d; // covariant copy: +1
    EXPECT_EQ(d.strongCount(), 2u);
    EXPECT_EQ(b.strongCount(), 2u);
    EXPECT_EQ(b->tag, 7);
    EXPECT_EQ(Base::alive, 1) << "still one logical object";
  }
  EXPECT_EQ(Base::alive, 0);
}

TEST_F(RcCovariantTest, MoveConvertDerivedToBase) {
  {
    xpp::Rc<Derived> d = xpp::makeRc<Derived>(7, 99);
    xpp::Rc<Base>    b = std::move(d); // count unchanged
    EXPECT_EQ(b.strongCount(), 1u);
    EXPECT_EQ(b->tag, 7);
  }
  EXPECT_EQ(Base::alive, 0);
}

/* ── Option<Rc<T>>: niche optimisation and Some/None semantics ───────── */

class OptionRcTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive = 0;
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive, 0) << "leak: " << Tracker::alive << " Trackers still alive";
  }
};

TEST_F(OptionRcTrackerTest, DefaultIsNone) {
  xpp::Option<xpp::Rc<Tracker>> o;
  EXPECT_TRUE(o.isNone());
  EXPECT_FALSE(o.isSome());
  EXPECT_FALSE(static_cast<bool>(o));
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRcTrackerTest, ConstructFromRcBumpsCount) {
  {
    xpp::Rc<Tracker> r = xpp::makeRc<Tracker>(8);
    EXPECT_EQ(r.strongCount(), 1u);
    {
      xpp::Option<xpp::Rc<Tracker>> o(r); // copy: +1
      EXPECT_TRUE(o.isSome());
      EXPECT_EQ(r.strongCount(), 2u);
      EXPECT_EQ(Tracker::alive, 1);
    } // o dies: -1
    EXPECT_EQ(r.strongCount(), 1u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRcTrackerTest, ConstructFromRcRvalueDoesNotBumpCount) {
  {
    xpp::Option<xpp::Rc<Tracker>> o(xpp::makeRc<Tracker>(8));
    EXPECT_TRUE(o.isSome());
    EXPECT_EQ(Tracker::alive, 1);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRcTrackerTest, AssignNoneDropsRc) {
  {
    xpp::Option<xpp::Rc<Tracker>> o(xpp::makeRc<Tracker>(1));
    EXPECT_EQ(Tracker::alive, 1);
    o = xpp::none;
    EXPECT_TRUE(o.isNone());
    EXPECT_EQ(Tracker::alive, 0) << "Tracker should be deleted when Option goes None";
  }
}

TEST_F(OptionRcTrackerTest, CopyOptionBumpsCount) {
  {
    xpp::Rc<Tracker>              r = xpp::makeRc<Tracker>(3);
    xpp::Option<xpp::Rc<Tracker>> o1(r);
    xpp::Option<xpp::Rc<Tracker>> o2 = o1; // copy: +1
    EXPECT_EQ(r.strongCount(), 3u);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRcTrackerTest, MoveOptionDoesNotBumpCount) {
  {
    xpp::Rc<Tracker>              r = xpp::makeRc<Tracker>(3);
    xpp::Option<xpp::Rc<Tracker>> o1(r); // count = 2
    EXPECT_EQ(r.strongCount(), 2u);
    xpp::Option<xpp::Rc<Tracker>> o2 = std::move(o1); // count unchanged
    EXPECT_EQ(r.strongCount(), 2u);
    EXPECT_TRUE(o1.isNone());
    EXPECT_TRUE(o2.isSome());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRcTrackerTest, TakeMovesOutRcCountUnchanged) {
  {
    xpp::Rc<Tracker>              r = xpp::makeRc<Tracker>(3);
    xpp::Option<xpp::Rc<Tracker>> o(r);    // count = 2
    EXPECT_EQ(r.strongCount(), 2u);

    xpp::Rc<Tracker> taken = o.take();     // moves out; count unchanged
    EXPECT_TRUE(o.isNone());
    EXPECT_EQ(r.strongCount(), 2u) << "take() moves ownership, does not bump or drop";
    EXPECT_EQ(taken.get(), r.get());
  }
  EXPECT_EQ(Tracker::alive, 0);
}

TEST_F(OptionRcTrackerTest, UnwrapOnRvalue) {
  {
    xpp::Option<xpp::Rc<Tracker>> o(xpp::makeRc<Tracker>(5)); // count = 1
    xpp::Rc<Tracker>              r = std::move(o).unwrap();
    EXPECT_EQ(r.strongCount(), 1u);
    EXPECT_EQ(r->value, 5);
  }
  EXPECT_EQ(Tracker::alive, 0);
}

} // namespace
