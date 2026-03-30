/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory_test.cpp - xAlloc/xFree/xRetain/xRelease/xCopy/xMove unit tests
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
#include <xbase/memory.h>
}

/* ── Test object ── */

struct Obj {
  int value;
};

/* ── Vtable call trackers ── */

struct Calls {
  std::atomic<int> ctor{0};
  std::atomic<int> dtor{0};
  std::atomic<int> retain{0};
  std::atomic<int> release{0};
  std::atomic<int> copy{0};
  std::atomic<int> move{0};

  void reset() {
    ctor = dtor = retain = release = copy = move = 0;
  }
};

static Calls g_calls;

static void obj_ctor(void *ptr)              { g_calls.ctor++;    }
static void obj_dtor(void *ptr)              { g_calls.dtor++;    }
static void obj_retain(void *ptr)            { g_calls.retain++;  }
static void obj_release(void *ptr)           { g_calls.release++; }
static void obj_copy(void *ptr, void *other) { g_calls.copy++;    }
static void obj_move(void *ptr, void *other) { g_calls.move++;    }

XDEF_VTABLE(Obj) {
  obj_ctor, obj_dtor, obj_retain, obj_release, obj_copy, obj_move,
};

/* Vtable with all NULL hooks */
static xVTable NullVTable = {0};

/* ── Fixture ── */

class MemoryTest : public ::testing::Test {
protected:
  void SetUp() override { g_calls.reset(); }
};

/* ========== xAlloc / xFree ========== */

TEST_F(MemoryTest, AllocReturnsNonNull) {
  Obj *o = XMALLOC(Obj);
  ASSERT_NE(o, nullptr);
  xFree(o);
}

TEST_F(MemoryTest, AllocCallsCtor) {
  Obj *o = XMALLOC(Obj);
  EXPECT_EQ(g_calls.ctor.load(), 1);
  xFree(o);
}

TEST_F(MemoryTest, FreeCallsDtor) {
  Obj *o = XMALLOC(Obj);
  g_calls.reset();
  xFree(o);
  EXPECT_EQ(g_calls.dtor.load(), 1);
}

TEST_F(MemoryTest, FreeNullIsNoop) {
  /* Should not crash */
  xFree(nullptr);
  EXPECT_EQ(g_calls.dtor.load(), 0);
}

TEST_F(MemoryTest, AllocNullVtableNoCrash) {
  /* ctor is NULL — should not crash */
  void *p = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  ASSERT_NE(p, nullptr);
  xFree(p);
}

TEST_F(MemoryTest, FreeNullVtableNoCrash) {
  /* dtor is NULL — should not crash */
  void *p = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  ASSERT_NE(p, nullptr);
  xFree(p);
  EXPECT_EQ(g_calls.dtor.load(), 0);
}

TEST_F(MemoryTest, AllocDataIsWritable) {
  Obj *o = XMALLOC(Obj);
  ASSERT_NE(o, nullptr);
  o->value = 42;
  EXPECT_EQ(o->value, 42);
  xFree(o);
}

TEST_F(MemoryTest, AllocExtraSize) {
  /* XMALLOCEX allocates sizeof(Obj) + extra bytes */
  Obj *o = XMALLOCEX(Obj, 64);
  ASSERT_NE(o, nullptr);
  /* Write into the extra region — should not crash */
  char *extra = reinterpret_cast<char *>(o) + sizeof(Obj);
  memset(extra, 0xAB, 64);
  EXPECT_EQ(static_cast<unsigned char>(extra[0]), 0xAB);
  xFree(o);
}

TEST_F(MemoryTest, AllocCountMultiple) {
  /* xAlloc with count=4 should give contiguous memory for 4 Objs */
  Obj *arr = static_cast<Obj *>(
      xAlloc("Obj", sizeof(Obj), 4, &ObjVTable));
  ASSERT_NE(arr, nullptr);
  EXPECT_EQ(g_calls.ctor.load(), 1);  /* ctor called once on the block */
  for (int i = 0; i < 4; i++) arr[i].value = i;
  for (int i = 0; i < 4; i++) EXPECT_EQ(arr[i].value, i);
  xFree(arr);
}

/* ========== xRetain / xRelease ========== */

TEST_F(MemoryTest, RetainCallsHook) {
  Obj *o = XMALLOC(Obj);
  g_calls.reset();
  xRetain(o);
  EXPECT_EQ(g_calls.retain.load(), 1);
  xRelease(o);  /* balance retain */
  xFree(o);
}

TEST_F(MemoryTest, ReleaseCallsHook) {
  Obj *o = XMALLOC(Obj);
  xRetain(o);
  g_calls.reset();
  xRelease(o);
  EXPECT_EQ(g_calls.release.load(), 0);  /* refs still > 0, no release hook */
  xFree(o);
}

TEST_F(MemoryTest, ReleaseToZeroCallsHookAndFrees) {
  Obj *o = XMALLOC(Obj);
  g_calls.reset();
  /* Initial refs = 1; Release to 0 should call release hook then xFree */
  xRelease(o);
  EXPECT_EQ(g_calls.release.load(), 1);
  EXPECT_EQ(g_calls.dtor.load(), 1);
}

TEST_F(MemoryTest, RetainReleasePaired) {
  Obj *o = XMALLOC(Obj);
  g_calls.reset();

  xRetain(o);   /* refs = 2 */
  xRetain(o);   /* refs = 3 */
  xRelease(o);  /* refs = 2 — no free */
  xRelease(o);  /* refs = 1 — no free */

  EXPECT_EQ(g_calls.dtor.load(), 0);

  xRelease(o);  /* refs = 0 — freed */
  EXPECT_EQ(g_calls.dtor.load(), 1);
}

TEST_F(MemoryTest, RetainNullVtableNoCrash) {
  void *p = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  ASSERT_NE(p, nullptr);
  xRetain(p);
  xRelease(p);  /* refs = 1 still, no free */
  xFree(p);
}

/* ========== xCopy / xMove ========== */

TEST_F(MemoryTest, CopyCallsHook) {
  Obj *a = XMALLOC(Obj);
  Obj *b = XMALLOC(Obj);
  g_calls.reset();
  xCopy(a, b);
  EXPECT_EQ(g_calls.copy.load(), 1);
  xFree(a);
  xFree(b);
}

TEST_F(MemoryTest, MoveCallsHook) {
  Obj *a = XMALLOC(Obj);
  Obj *b = XMALLOC(Obj);
  g_calls.reset();
  xMove(a, b);
  EXPECT_EQ(g_calls.move.load(), 1);
  xFree(a);
  xFree(b);
}

TEST_F(MemoryTest, CopyNullVtableNoCrash) {
  void *a = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  void *b = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  xCopy(a, b);  /* copy is NULL — should not crash */
  EXPECT_EQ(g_calls.copy.load(), 0);
  xFree(a);
  xFree(b);
}

TEST_F(MemoryTest, MoveNullVtableNoCrash) {
  void *a = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  void *b = xAlloc("test", sizeof(Obj), 1, &NullVTable);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  xMove(a, b);  /* move is NULL — should not crash */
  EXPECT_EQ(g_calls.move.load(), 0);
  xFree(a);
  xFree(b);
}

/* ========== Thread Safety ========== */

TEST_F(MemoryTest, ConcurrentRetainRelease) {
  constexpr int NTHREADS  = 8;
  constexpr int NRETAINS  = 10000;

  Obj *o = XMALLOC(Obj);
  g_calls.reset();

  std::vector<std::thread> threads;
  for (int t = 0; t < NTHREADS; t++) {
    threads.emplace_back([o]() {
      for (int i = 0; i < NRETAINS; i++) {
        xRetain(o);
      }
      for (int i = 0; i < NRETAINS; i++) {
        xRelease(o);
      }
    });
  }

  for (auto &th : threads) th.join();

  /* All extra retains balanced — one xRelease should free it */
  g_calls.reset();
  xRelease(o);
  EXPECT_EQ(g_calls.dtor.load(), 1);
}

TEST_F(MemoryTest, ConcurrentAllocFree) {
  constexpr int NTHREADS = 4;
  constexpr int NALLOCS  = 5000;

  std::atomic<int> alive{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < NTHREADS; t++) {
    threads.emplace_back([&alive]() {
      for (int i = 0; i < NALLOCS; i++) {
        Obj *o = XMALLOC(Obj);
        alive.fetch_add(1, std::memory_order_relaxed);
        o->value = 42;
        alive.fetch_sub(1, std::memory_order_relaxed);
        xFree(o);
      }
    });
  }

  for (auto &th : threads) th.join();
  EXPECT_EQ(alive.load(), 0);
}

TEST_F(MemoryTest, ConcurrentRetainReleaseAcrossThreads) {
  constexpr int NRETAINERS = 4;
  constexpr int NRELEASES  = 10000;

  Obj *o = XMALLOC(Obj);
  /* Bump refs so each thread can release NRELEASES times */
  for (int i = 0; i < NRETAINERS * NRELEASES; i++) {
    xRetain(o);
  }

  std::vector<std::thread> threads;
  for (int t = 0; t < NRETAINERS; t++) {
    threads.emplace_back([o, NRELEASES]() {
      for (int i = 0; i < NRELEASES; i++) {
        xRelease(o);
      }
    });
  }

  for (auto &th : threads) th.join();

  /* All extra retains balanced — one xRelease should free it */
  g_calls.reset();
  xRelease(o);
  EXPECT_EQ(g_calls.dtor.load(), 1);
}

/* ========== XMALLOC macro ========== */

TEST_F(MemoryTest, XMallocMacroCallsCtor) {
  Obj *o = XMALLOC(Obj);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(g_calls.ctor.load(), 1);
  xFree(o);
}

TEST_F(MemoryTest, XMallocExMacroCallsCtor) {
  Obj *o = XMALLOCEX(Obj, 32);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(g_calls.ctor.load(), 1);
  xFree(o);
}
