/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * heap_test.cpp - xHeap unit tests
 */

#include <gtest/gtest.h>

#include <cstdlib>
#include <vector>

extern "C" {
#include <xbase/heap.h>
}

/* ── Test element ── */

struct Elem {
  int    value;
  size_t heap_idx;
};

static int elem_cmp(const void *a, const void *b) {
  int va = static_cast<const Elem *>(a)->value;
  int vb = static_cast<const Elem *>(b)->value;
  return (va > vb) - (va < vb);
}

static void elem_setidx(void *e, size_t idx) {
  static_cast<Elem *>(e)->heap_idx = idx;
}

/* ── Fixture ── */

class HeapTest : public ::testing::Test {
protected:
  xHeap h = nullptr;

  void SetUp() override {
    h = xHeapCreate(elem_cmp, elem_setidx, 0);
    ASSERT_NE(h, nullptr);
  }

  void TearDown() override {
    if (h) xHeapDestroy(h);
  }
};

/* ========== Basic ========== */

TEST_F(HeapTest, CreateAndDestroy) {
  EXPECT_EQ(xHeapSize(h), 0u);
  EXPECT_EQ(xHeapPeek(h), nullptr);
  EXPECT_EQ(xHeapPop(h), nullptr);
}

TEST_F(HeapTest, PushSingleAndPop) {
  Elem e = {42, 0};
  EXPECT_EQ(xHeapPush(h, &e), xErrno_Ok);
  EXPECT_EQ(xHeapSize(h), 1u);
  EXPECT_EQ(xHeapPeek(h), &e);

  void *popped = xHeapPop(h);
  EXPECT_EQ(popped, &e);
  EXPECT_EQ(xHeapSize(h), 0u);
}

TEST_F(HeapTest, MinProperty) {
  Elem elems[] = {{5, 0}, {3, 0}, {8, 0}, {1, 0}, {4, 0}};
  for (auto &e : elems) {
    xHeapPush(h, &e);
  }

  EXPECT_EQ(xHeapSize(h), 5u);

  /* Pop should yield elements in ascending order */
  int prev = -1;
  while (xHeapSize(h) > 0) {
    Elem *e = static_cast<Elem *>(xHeapPop(h));
    EXPECT_GE(e->value, prev);
    prev = e->value;
  }
}

TEST_F(HeapTest, DuplicateValues) {
  Elem elems[] = {{3, 0}, {3, 0}, {3, 0}};
  for (auto &e : elems) {
    xHeapPush(h, &e);
  }

  EXPECT_EQ(xHeapSize(h), 3u);
  for (int i = 0; i < 3; i++) {
    Elem *e = static_cast<Elem *>(xHeapPop(h));
    EXPECT_EQ(e->value, 3);
  }
}

/* ========== Remove ========== */

TEST_F(HeapTest, RemoveMiddle) {
  Elem elems[] = {{1, 0}, {5, 0}, {3, 0}, {7, 0}, {2, 0}};
  for (auto &e : elems) {
    xHeapPush(h, &e);
  }

  /* Remove the element with value 5 */
  Elem *target = nullptr;
  for (auto &e : elems) {
    if (e.value == 5) {
      target = &e;
      break;
    }
  }
  ASSERT_NE(target, nullptr);

  void *removed = xHeapRemove(h, target->heap_idx);
  EXPECT_EQ(removed, target);
  EXPECT_EQ(xHeapSize(h), 4u);

  /* Remaining should still be a valid min-heap */
  int prev = -1;
  while (xHeapSize(h) > 0) {
    Elem *e = static_cast<Elem *>(xHeapPop(h));
    EXPECT_GE(e->value, prev);
    EXPECT_NE(e->value, 5);
    prev = e->value;
  }
}

TEST_F(HeapTest, RemoveMin) {
  Elem elems[] = {{1, 0}, {5, 0}, {3, 0}};
  for (auto &e : elems) {
    xHeapPush(h, &e);
  }

  void *removed = xHeapRemove(h, 0);
  EXPECT_EQ(static_cast<Elem *>(removed)->value, 1);
  EXPECT_EQ(xHeapSize(h), 2u);

  Elem *next_min = static_cast<Elem *>(xHeapPeek(h));
  EXPECT_EQ(next_min->value, 3);
}

/* ========== Update ========== */

TEST_F(HeapTest, UpdateDecreasePriority) {
  Elem elems[] = {{5, 0}, {3, 0}, {8, 0}};
  for (auto &e : elems) {
    xHeapPush(h, &e);
  }

  /* Decrease the value of 8 to 1 — should become the new min */
  elems[2].value = 1;
  xHeapUpdate(h, elems[2].heap_idx);

  Elem *min = static_cast<Elem *>(xHeapPeek(h));
  EXPECT_EQ(min->value, 1);
}

TEST_F(HeapTest, UpdateIncreasePriority) {
  Elem elems[] = {{1, 0}, {3, 0}, {5, 0}};
  for (auto &e : elems) {
    xHeapPush(h, &e);
  }

  /* Increase the value of 1 to 10 — should no longer be min */
  elems[0].value = 10;
  xHeapUpdate(h, elems[0].heap_idx);

  Elem *min = static_cast<Elem *>(xHeapPeek(h));
  EXPECT_EQ(min->value, 3);
}

/* ========== Large scale ========== */

TEST_F(HeapTest, LargeScaleSorted) {
  constexpr int     N = 1000;
  std::vector<Elem> elems(N);

  /* Insert in random order */
  for (int i = 0; i < N; i++) {
    elems[i].value = rand() % 10000;
    xHeapPush(h, &elems[i]);
  }

  EXPECT_EQ(xHeapSize(h), (size_t)N);

  /* Pop all — should be sorted */
  int prev = -1;
  for (int i = 0; i < N; i++) {
    Elem *e = static_cast<Elem *>(xHeapPop(h));
    ASSERT_NE(e, nullptr);
    EXPECT_GE(e->value, prev);
    prev = e->value;
  }
}

/* ========== Edge cases ========== */

TEST_F(HeapTest, NullArgs) {
  EXPECT_EQ(xHeapCreate(nullptr, elem_setidx, 0), nullptr);
  EXPECT_EQ(xHeapCreate(elem_cmp, nullptr, 0), nullptr);
  EXPECT_EQ(xHeapPush(nullptr, nullptr), xErrno_InvalidArg);
  EXPECT_EQ(xHeapPeek(nullptr), nullptr);
  EXPECT_EQ(xHeapPop(nullptr), nullptr);
  EXPECT_EQ(xHeapRemove(nullptr, 0), nullptr);
  EXPECT_EQ(xHeapSize(nullptr), 0u);
}

TEST_F(HeapTest, RemoveOutOfRange) {
  Elem e = {1, 0};
  xHeapPush(h, &e);
  EXPECT_EQ(xHeapRemove(h, 99), nullptr);
}
