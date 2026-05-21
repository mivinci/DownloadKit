/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * arc_test.cpp - Tests for Arc<T>, Option<Arc<T>>, and ArcWeak<T>.
 *
 * Covers the same surface as rc_test.cpp + weak_test.cpp, plus a
 * concurrency smoke test: N threads each clone + drop an Arc M
 * times; final strong count must equal the number of survivors, no
 * leaks. ASan / TSan in CI catches torn counts and use-after-free
 * the deterministic Tracker count assertion wouldn't notice on its
 * own.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <xpp/arc.h>
#include <xpp/option.h>

/* ── Compile-time guarantees ─────────────────────────────────────────── */

static_assert(sizeof(xpp::Arc<int>) == sizeof(int *), "Arc<int> must be sizeof(int*)");
static_assert(sizeof(xpp::Option<xpp::Arc<int>>) == sizeof(int *),
              "Option<Arc<int>> niche optimisation broken");
static_assert(sizeof(xpp::ArcWeak<int>) == sizeof(int *), "ArcWeak<int> must be sizeof(int*)");

static_assert(std::is_copy_constructible<xpp::Arc<int>>::value, "Arc must be copyable");
static_assert(std::is_move_constructible<xpp::Arc<int>>::value, "Arc must be movable");
static_assert(!std::is_default_constructible<xpp::Arc<int>>::value,
              "Arc must NOT be default-constructible (always non-null)");
static_assert(std::is_default_constructible<xpp::ArcWeak<int>>::value,
              "ArcWeak must default-construct (yields null)");

namespace {

/* ── Tracker fixture ────────────────────────────────────────────────── */

struct Tracker {
  static std::atomic<int> alive;
  int                     value;

  explicit Tracker(int v = 0) : value(v) {
    alive.fetch_add(1, std::memory_order_relaxed);
  }
  Tracker(const Tracker &)            = delete;
  Tracker(Tracker &&)                 = delete;
  Tracker &operator=(const Tracker &) = delete;
  Tracker &operator=(Tracker &&)      = delete;
  ~Tracker() {
    alive.fetch_sub(1, std::memory_order_relaxed);
  }
};
std::atomic<int> Tracker::alive{0};

class ArcTrackerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Tracker::alive.store(0, std::memory_order_relaxed);
  }
  void TearDown() override {
    EXPECT_EQ(Tracker::alive.load(), 0) << "leak: Trackers still alive";
  }
};

/* ── Basics: Arc::make, copy/move, strong count, drop ─────────────────── */

TEST_F(ArcTrackerTest, MakeArcCountStartsAtOne) {
  {
    xpp::Arc<Tracker> a = xpp::Arc<Tracker>::make(42);
    EXPECT_EQ(a->value, 42);
    EXPECT_EQ(a.strong_count(), 1u);
    EXPECT_EQ(a.weak_count(), 0u);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

TEST_F(ArcTrackerTest, CopyBumpsStrong) {
  {
    xpp::Arc<Tracker> a = xpp::Arc<Tracker>::make(7);
    {
      xpp::Arc<Tracker> b = a;
      EXPECT_EQ(a.strong_count(), 2u);
      EXPECT_EQ(b.strong_count(), 2u);
      EXPECT_EQ(a.get(), b.get());
    }
    EXPECT_EQ(a.strong_count(), 1u);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

TEST_F(ArcTrackerTest, MoveDoesNotBumpStrong) {
  {
    xpp::Arc<Tracker> a = xpp::Arc<Tracker>::make(5);
    xpp::Arc<Tracker> b = std::move(a);
    EXPECT_EQ(b.strong_count(), 1u);
    EXPECT_EQ(b->value, 5);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

TEST_F(ArcTrackerTest, CloneEqualsCopy) {
  {
    xpp::Arc<Tracker> a = xpp::Arc<Tracker>::make(1);
    xpp::Arc<Tracker> b = a.clone();
    xpp::Arc<Tracker> c = xpp::Arc<Tracker>::clone(a); // Rust spelling
    EXPECT_EQ(a.strong_count(), 3u);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

/* ── Option<Arc<T>> niche ────────────────────────────────────────────── */

TEST_F(ArcTrackerTest, OptionArcDefaultIsNone) {
  xpp::Option<xpp::Arc<Tracker>> o;
  EXPECT_TRUE(o.is_none());
}

TEST_F(ArcTrackerTest, OptionArcFromArcBumpsStrong) {
  {
    xpp::Arc<Tracker>              a = xpp::Arc<Tracker>::make(3);
    xpp::Option<xpp::Arc<Tracker>> o(a);
    EXPECT_TRUE(o.is_some());
    EXPECT_EQ(a.strong_count(), 2u);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

TEST_F(ArcTrackerTest, OptionArcTakeDoesNotChangeStrong) {
  {
    xpp::Arc<Tracker>              a = xpp::Arc<Tracker>::make(8);
    xpp::Option<xpp::Arc<Tracker>> o(a);
    EXPECT_EQ(a.strong_count(), 2u);

    xpp::Arc<Tracker> taken = o.take();
    EXPECT_TRUE(o.is_none());
    EXPECT_EQ(a.strong_count(), 2u) << "take is move, not clone";
    EXPECT_EQ(taken.get(), a.get());
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

/* ── ArcWeak basics ──────────────────────────────────────────────────── */

TEST_F(ArcTrackerTest, ArcWeakDefaultIsNull) {
  xpp::ArcWeak<Tracker> w;
  EXPECT_TRUE(w.is_expired());
  EXPECT_TRUE(w.upgrade().is_none());
}

TEST_F(ArcTrackerTest, ArcDowngradeAndUpgrade) {
  {
    xpp::Arc<Tracker>     a = xpp::Arc<Tracker>::make(11);
    xpp::ArcWeak<Tracker> w = xpp::Arc<Tracker>::downgrade(a);
    EXPECT_EQ(a.weak_count(), 1u);
    EXPECT_FALSE(w.is_expired());

    xpp::Option<xpp::Arc<Tracker>> upgraded = w.upgrade();
    ASSERT_TRUE(upgraded.is_some());
    EXPECT_EQ(a.strong_count(), 2u);

    xpp::Arc<Tracker> b = std::move(upgraded).unwrap();
    EXPECT_EQ(b->value, 11);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

TEST_F(ArcTrackerTest, ArcWeakUpgradeAfterAllStrongsGoneIsNone) {
  xpp::ArcWeak<Tracker> w;
  {
    xpp::Arc<Tracker> a = xpp::Arc<Tracker>::make(13);
    w                   = xpp::ArcWeak<Tracker>(a);
  } // strong gone, Tracker destroyed
  EXPECT_EQ(Tracker::alive.load(), 0);
  EXPECT_TRUE(w.is_expired());
  EXPECT_TRUE(w.upgrade().is_none());
}

TEST_F(ArcTrackerTest, ArcWeakOutlivingAllStrongsDoesNotLeakInner) {
  {
    xpp::ArcWeak<Tracker> w;
    {
      xpp::Arc<Tracker> a = xpp::Arc<Tracker>::make(17);
      w                   = xpp::ArcWeak<Tracker>(a);
    }
    EXPECT_EQ(Tracker::alive.load(), 0);
    EXPECT_TRUE(w.is_expired());
  } // w dies → inner deallocated; ASan in CI confirms no leak
  EXPECT_EQ(Tracker::alive.load(), 0);
}

/* ── Concurrency smoke test ──────────────────────────────────────────── */

TEST_F(ArcTrackerTest, ClonesAndDropsAcrossThreadsDoNotLeak) {
  constexpr int k_threads        = 8;
  constexpr int k_iterations     = 10000;

  {
    xpp::Arc<Tracker>        root = xpp::Arc<Tracker>::make(0);
    std::vector<std::thread> ts;
    ts.reserve(k_threads);

    for (int i = 0; i < k_threads; ++i) {
      ts.emplace_back([root]() {
        // Each thread takes a copy on entry, then clones & drops in a
        // loop. If any drop/clone races corrupt the count, total
        // alive Trackers at the end won't be 1.
        for (int j = 0; j < k_iterations; ++j) {
          xpp::Arc<Tracker> local = root.clone();
          (void) local; // dropped immediately
        }
      });
    }
    for (auto &t : ts) t.join();

    // After every thread joined, only `root` should hold the Arc.
    EXPECT_EQ(root.strong_count(), 1u);
    EXPECT_EQ(Tracker::alive.load(), 1);
  }
  EXPECT_EQ(Tracker::alive.load(), 0);
}

TEST_F(ArcTrackerTest, UpgradeRacesWithLastDropEitherSomeOrNone) {
  // Many threads each race to upgrade against a single dropping
  // thread. Result of any single upgrade is either Some (we got
  // there before the last drop completed) or None (we didn't), and
  // either is OK — what matters is no UB / no count drift.
  constexpr int k_threads = 8;

  for (int trial = 0; trial < 200; ++trial) {
    xpp::Arc<Tracker>     a = xpp::Arc<Tracker>::make(trial);
    xpp::ArcWeak<Tracker> w = xpp::Arc<Tracker>::downgrade(a);

    std::atomic<int>         observed_some{0};
    std::vector<std::thread> ts;
    ts.reserve(k_threads);

    for (int i = 0; i < k_threads; ++i) {
      ts.emplace_back([&w, &observed_some]() {
        xpp::Option<xpp::Arc<Tracker>> r = w.upgrade();
        if (r.is_some()) {
          observed_some.fetch_add(1, std::memory_order_relaxed);
          // dropped here
        }
      });
    }

    // Main thread drops the only strong reference concurrent with
    // the upgrade attempts.
    a = xpp::Arc<Tracker>::make(-1); // overwrite drops original
    (void) a;                      // suppress unused warning in release

    for (auto &t : ts) t.join();
    // No specific count to assert beyond "no UB and no leak" — both
    // are checked by ASan/TSan + the TearDown Tracker count.
    (void) observed_some;
  }
  // Tracker count returns to 0 once a falls out of scope on each iteration.
}

} // namespace
