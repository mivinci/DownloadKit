/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT style license that can be
 * found in the LICENSE file.
 *
 * array_test.cpp - xArray unit tests
 */

#include <gtest/gtest.h>

#include <cstdlib>

extern "C" {
#include <xbase/array.h>
}

/* ── Test element ── */

struct Pod {
  int  x;
  int  y;
};

struct Heap {
  char *data;
};

/* ── Callbacks ── */

static int g_retain_count = 0;
static int g_release_count = 0;

static void pod_retain(void *elem) {
  (void)elem;
  g_retain_count++;
}

static void pod_release(void *elem) {
  (void)elem;
  g_release_count++;
}

static void heap_release(void *elem) {
  struct Heap *h = static_cast<struct Heap *>(elem);
  free(h->data);
  h->data = nullptr;
}

static int pod_equal_x(const void *elem, const void *key) {
  const Pod *e  = static_cast<const Pod *>(elem);
  const int *k  = static_cast<const int *>(key);
  return e->x == *k;
}

/* ── Callback sets ── */

static const xArrayCallbacks kPodCbs = { pod_retain, pod_release, pod_equal_x };
static const xArrayCallbacks kHeapCbs = { nullptr, heap_release, nullptr };
static const xArrayCallbacks kNoCbs = { nullptr, nullptr, nullptr };

/* ── Fixture ── */

class ArrayTest : public ::testing::Test {
protected:
  xArray arr = nullptr;

  void SetUp() override {
    g_retain_count  = 0;
    g_release_count = 0;
    arr = xArrayCreate(sizeof(Pod), 4, &kPodCbs);
    ASSERT_NE(arr, nullptr);
  }

  void TearDown() override {
    if (arr) xArrayDestroy(arr);
  }
};

/* ========== Basic lifecycle ========== */

TEST_F(ArrayTest, CreateAndDestroy) {
  EXPECT_EQ(xArrayLen(arr), 0u);
  EXPECT_EQ(xArrayCap(arr), 4u);
  EXPECT_EQ(xArrayData(arr), nullptr);
  EXPECT_EQ(xArrayAt(arr, 0), nullptr);
}

TEST_F(ArrayTest, CreateZeroElemSize) {
  xArray a = xArrayCreate(0, 4, nullptr);
  EXPECT_EQ(a, nullptr);
}

TEST_F(ArrayTest, CreateDefaultCap) {
  xArray a = xArrayCreate(sizeof(int), 0, nullptr);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(xArrayCap(a), 8u); /* ARRAY_DEFAULT_CAP */
  xArrayDestroy(a);
}

TEST_F(ArrayTest, CreateNullCbs) {
  xArray a = xArrayCreate(sizeof(Pod), 4, nullptr);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(xArrayLen(a), 0u);
  xArrayDestroy(a);
}

/* ========== Push ========== */

TEST_F(ArrayTest, PushSingle) {
  Pod *slot = (Pod *)xArrayPush(&arr);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->x, 0);
  EXPECT_EQ(slot->y, 0);
  slot->x = 10;
  slot->y = 20;
  EXPECT_EQ(xArrayLen(arr), 1u);
  EXPECT_EQ(g_retain_count, 1);

  Pod *got = (Pod *)xArrayAt(arr, 0);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->x, 10);
  EXPECT_EQ(got->y, 20);
}

TEST_F(ArrayTest, PushMultiple) {
  for (int i = 0; i < 10; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    ASSERT_NE(slot, nullptr);
    slot->x = i;
    slot->y = i * 10;
  }
  EXPECT_EQ(xArrayLen(arr), 10u);
  EXPECT_EQ(g_retain_count, 10);

  for (int i = 0; i < 10; i++) {
    Pod *e = (Pod *)xArrayAt(arr, (size_t)i);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->x, i);
    EXPECT_EQ(e->y, i * 10);
  }
}

TEST_F(ArrayTest, PushTriggersGrowth) {
  /* Initial cap is 4, push 5 elements to trigger growth */
  for (int i = 0; i < 5; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    ASSERT_NE(slot, nullptr);
    slot->x = i;
  }
  EXPECT_GE(xArrayCap(arr), 5u);
  EXPECT_EQ(xArrayLen(arr), 5u);

  /* Verify all elements are still correct after realloc */
  for (int i = 0; i < 5; i++) {
    Pod *e = (Pod *)xArrayAt(arr, (size_t)i);
    EXPECT_EQ(e->x, i);
  }
}

/* ========== Pop ========== */

TEST_F(ArrayTest, PopBasic) {
  Pod *slot = (Pod *)xArrayPush(&arr);
  slot->x = 42;
  EXPECT_EQ(xArrayLen(arr), 1u);

  xErrno err = xArrayPop(arr);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xArrayLen(arr), 0u);
  EXPECT_EQ(g_release_count, 1);
}

TEST_F(ArrayTest, PopEmpty) {
  xErrno err = xArrayPop(arr);
  EXPECT_EQ(err, xErrno_InvalidState);
}

TEST_F(ArrayTest, PopWithRelease) {
  xArray ha = xArrayCreate(sizeof(Heap), 4, &kHeapCbs);
  ASSERT_NE(ha, nullptr);

  Heap *slot = (Heap *)xArrayPush(&ha);
  slot->data = strdup("hello");

  EXPECT_EQ(xArrayLen(ha), 1u);
  xArrayPop(ha);
  EXPECT_EQ(xArrayLen(ha), 0u);

  xArrayDestroy(ha);
}

/* ========== Reset ========== */

TEST_F(ArrayTest, Reset) {
  for (int i = 0; i < 3; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    slot->x = i;
  }
  EXPECT_EQ(xArrayLen(arr), 3u);

  xArrayReset(arr);
  EXPECT_EQ(xArrayLen(arr), 0u);
  /* Capacity should be preserved */
  EXPECT_EQ(xArrayCap(arr), 4u);
  /* Release should have been called for each element */
  EXPECT_EQ(g_release_count, 3);
}

TEST_F(ArrayTest, ResetWithRelease) {
  xArray ha = xArrayCreate(sizeof(Heap), 4, &kHeapCbs);

  for (int i = 0; i < 3; i++) {
    Heap *slot = (Heap *)xArrayPush(&ha);
    char buf[16];
    snprintf(buf, sizeof(buf), "item%d", i);
    slot->data = strdup(buf);
  }

  xArrayReset(ha);
  EXPECT_EQ(xArrayLen(ha), 0u);

  xArrayDestroy(ha);
}

/* ========== Resize ========== */

TEST_F(ArrayTest, ResizeGrow) {
  xErrno err = xArrayResize(&arr, 10);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xArrayLen(arr), 10u);
  /* Retain should have been called for each new element */
  EXPECT_EQ(g_retain_count, 10);

  /* New slots should be zero-initialized */
  for (size_t i = 0; i < 10; i++) {
    Pod *e = (Pod *)xArrayAt(arr, i);
    EXPECT_EQ(e->x, 0);
    EXPECT_EQ(e->y, 0);
  }
}

TEST_F(ArrayTest, ResizeShrink) {
  for (int i = 0; i < 5; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    slot->x = i;
  }

  xErrno err = xArrayResize(&arr, 2);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xArrayLen(arr), 2u);
  /* Release should have been called for 3 removed elements */
  EXPECT_EQ(g_release_count, 3);

  EXPECT_EQ(((Pod *)xArrayAt(arr, 0))->x, 0);
  EXPECT_EQ(((Pod *)xArrayAt(arr, 1))->x, 1);
  EXPECT_EQ(xArrayAt(arr, 2), nullptr);
}

TEST_F(ArrayTest, ResizeShrinkWithRelease) {
  xArray ha = xArrayCreate(sizeof(Heap), 4, &kHeapCbs);

  for (int i = 0; i < 5; i++) {
    Heap *slot = (Heap *)xArrayPush(&ha);
    char buf[16];
    snprintf(buf, sizeof(buf), "item%d", i);
    slot->data = strdup(buf);
  }

  xArrayResize(&ha, 2);
  EXPECT_EQ(xArrayLen(ha), 2u);

  xArrayDestroy(ha);
}

/* ========== Find ========== */

TEST_F(ArrayTest, FindBasic) {
  for (int i = 0; i < 5; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    slot->x = i * 10;
  }

  int key = 20;
  size_t idx = xArrayFind(arr, &key);
  EXPECT_EQ(idx, 2u);

  key = 99;
  idx = xArrayFind(arr, &key);
  EXPECT_EQ(idx, (size_t)-1);
}

TEST_F(ArrayTest, FindNoEqualCb) {
  xArray a = xArrayCreate(sizeof(Pod), 4, &kNoCbs);
  int key = 0;
  EXPECT_EQ(xArrayFind(a, &key), (size_t)-1);
  xArrayDestroy(a);
}

/* ========== Accessors ========== */

TEST_F(ArrayTest, AtOutOfRange) {
  EXPECT_EQ(xArrayAt(arr, 0), nullptr);
  EXPECT_EQ(xArrayAt(arr, 99), nullptr);
}

TEST_F(ArrayTest, Data) {
  Pod *s1 = (Pod *)xArrayPush(&arr);
  s1->x = 1;
  Pod *s2 = (Pod *)xArrayPush(&arr);
  s2->x = 2;

  Pod *base = (Pod *)xArrayData(arr);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base[0].x, 1);
  EXPECT_EQ(base[1].x, 2);
}

/* ========== Edge cases ========== */

TEST_F(ArrayTest, NullArgs) {
  EXPECT_EQ(xArrayCreate(0, 4, nullptr), nullptr);

  xArrayDestroy(nullptr);
  xArrayReset(nullptr);

  EXPECT_EQ(xArrayPush(nullptr), nullptr);
  EXPECT_EQ(xArrayPop(nullptr), xErrno_InvalidState);
  EXPECT_EQ(xArrayResize(nullptr, 10), xErrno_InvalidArg);
  EXPECT_EQ(xArrayAt(nullptr, 0), nullptr);
  EXPECT_EQ(xArrayLen(nullptr), 0u);
  EXPECT_EQ(xArrayCap(nullptr), 0u);
  EXPECT_EQ(xArrayData(nullptr), nullptr);
  EXPECT_EQ(xArrayFind(nullptr, nullptr), (size_t)-1);
}

/* ========== Large scale ========== */

TEST_F(ArrayTest, LargeScalePush) {
  constexpr int N = 10000;

  for (int i = 0; i < N; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    ASSERT_NE(slot, nullptr);
    slot->x = i;
    slot->y = i * 2;
  }
  EXPECT_EQ(xArrayLen(arr), (size_t)N);
  EXPECT_EQ(g_retain_count, N);

  for (int i = 0; i < N; i++) {
    Pod *e = (Pod *)xArrayAt(arr, (size_t)i);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->x, i);
    EXPECT_EQ(e->y, i * 2);
  }
}

TEST_F(ArrayTest, PushAndPopCycle) {
  /* Push 100, pop 50, push 50 more - verify integrity */
  for (int i = 0; i < 100; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    slot->x = i;
  }
  EXPECT_EQ(xArrayLen(arr), 100u);
  EXPECT_EQ(g_retain_count, 100);

  for (int i = 0; i < 50; i++) {
    xArrayPop(arr);
  }
  EXPECT_EQ(xArrayLen(arr), 50u);
  EXPECT_EQ(g_release_count, 50);

  /* First 50 elements should still be intact */
  for (int i = 0; i < 50; i++) {
    Pod *e = (Pod *)xArrayAt(arr, (size_t)i);
    EXPECT_EQ(e->x, i);
  }

  /* Push 50 more */
  for (int i = 100; i < 150; i++) {
    Pod *slot = (Pod *)xArrayPush(&arr);
    slot->x = i;
  }
  EXPECT_EQ(xArrayLen(arr), 100u);
  EXPECT_EQ(g_retain_count, 150);
}
