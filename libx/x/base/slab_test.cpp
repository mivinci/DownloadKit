/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slab_test.cpp - xSlab / xSlabMt unit tests
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

extern "C" {
#include <x/base/slab.h>
}

namespace {

struct Small {
  int a;
  int b;
};

struct Large {
  char buf[256];
};

bool is_aligned(const void *p, size_t a) {
  return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
}

} // namespace

/* ========== xSlab basics ========== */

TEST(SlabTest, CreateDestroy) {
  xSlab *s = xSlabCreate(sizeof(Small), 0, 0);
  ASSERT_NE(s, nullptr);
  EXPECT_GE(xSlabSlotSize(s), sizeof(Small));
  EXPECT_EQ(xSlabInUse(s), 0u);
  xSlabDestroy(s);
}

TEST(SlabTest, InvalidArgs) {
  EXPECT_EQ(xSlabCreate(0, 0, 0), nullptr);
  EXPECT_EQ(xSlabCreate(sizeof(Small), 3, 0), nullptr);
  EXPECT_EQ(xSlabCreate(sizeof(Small), 7, 0), nullptr);
  xSlabDestroy(nullptr);
  xSlabFree(nullptr, nullptr);
  EXPECT_EQ(xSlabAlloc(nullptr), nullptr);
  EXPECT_EQ(xSlabInUse(nullptr), 0u);
  EXPECT_EQ(xSlabSlotSize(nullptr), 0u);
}

TEST(SlabTest, AllocFreeRoundTrip) {
  xSlab *s = xSlabCreate(sizeof(Small), 16, 0);
  ASSERT_NE(s, nullptr);

  void *p = xSlabAlloc(s);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(is_aligned(p, 16));
  EXPECT_EQ(xSlabInUse(s), 1u);

  std::memset(p, 0xAB, sizeof(Small));

  xSlabFree(s, p);
  EXPECT_EQ(xSlabInUse(s), 0u);

  void *p2 = xSlabAlloc(s);
  EXPECT_EQ(p2, p);
  xSlabFree(s, p2);

  xSlabDestroy(s);
}

TEST(SlabTest, AllocAcrossChunks) {
  xSlab *s = xSlabCreate(sizeof(Large), 16, 1024);
  ASSERT_NE(s, nullptr);

  constexpr int       N = 200;
  std::vector<void *> ptrs;
  ptrs.reserve(N);

  std::unordered_set<void *> seen;
  for (int i = 0; i < N; i++) {
    void *p = xSlabAlloc(s);
    ASSERT_NE(p, nullptr) << "alloc " << i;
    EXPECT_TRUE(is_aligned(p, 16));
    EXPECT_TRUE(seen.insert(p).second) << "duplicate pointer at " << i;
    std::memset(p, i & 0xFF, sizeof(Large));
    ptrs.push_back(p);
  }
  EXPECT_EQ(xSlabInUse(s), (size_t)N);

  for (void *p : ptrs) xSlabFree(s, p);
  EXPECT_EQ(xSlabInUse(s), 0u);

  xSlabDestroy(s);
}

TEST(SlabTest, AlignmentRespected) {
  for (size_t a : {size_t{8}, size_t{16}, size_t{32}, size_t{64},
                   size_t{128}}) {
    xSlab *s = xSlabCreate(sizeof(Small), a, 0);
    ASSERT_NE(s, nullptr) << "align=" << a;
    for (int i = 0; i < 64; i++) {
      void *p = xSlabAlloc(s);
      ASSERT_NE(p, nullptr);
      EXPECT_TRUE(is_aligned(p, a)) << "align=" << a << " iter=" << i;
    }
    xSlabDestroy(s);
  }
}

TEST(SlabTest, TinyObjectPromotedToPointerSize) {
  xSlab *s = xSlabCreate(1, 0, 0);
  ASSERT_NE(s, nullptr);
  EXPECT_GE(xSlabSlotSize(s), sizeof(void *));
  void *p = xSlabAlloc(s);
  ASSERT_NE(p, nullptr);
  xSlabFree(s, p);
  xSlabDestroy(s);
}

TEST(SlabTest, Reset) {
  xSlab *s = xSlabCreate(sizeof(Small), 16, 512);
  ASSERT_NE(s, nullptr);

  std::vector<void *> ptrs;
  for (int i = 0; i < 50; i++) ptrs.push_back(xSlabAlloc(s));
  EXPECT_EQ(xSlabInUse(s), 50u);

  xSlabReset(s);
  EXPECT_EQ(xSlabInUse(s), 0u);

  for (int i = 0; i < 50; i++) {
    void *p = xSlabAlloc(s);
    EXPECT_NE(p, nullptr);
  }
  xSlabDestroy(s);
}

TEST(SlabTest, ReuseOrderIsLifo) {
  xSlab *s = xSlabCreate(sizeof(Small), 0, 0);
  ASSERT_NE(s, nullptr);

  void *a = xSlabAlloc(s);
  void *b = xSlabAlloc(s);
  void *c = xSlabAlloc(s);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  ASSERT_NE(c, nullptr);

  xSlabFree(s, a);
  xSlabFree(s, b);
  xSlabFree(s, c);

  EXPECT_EQ(xSlabAlloc(s), c);
  EXPECT_EQ(xSlabAlloc(s), b);
  EXPECT_EQ(xSlabAlloc(s), a);

  xSlabDestroy(s);
}

/* ========== xSlabMt ========== */

TEST(SlabMtTest, CreateDestroy) {
  xSlabMt *s = xSlabMtCreate(sizeof(Small), 0, 0);
  ASSERT_NE(s, nullptr);
  EXPECT_GE(xSlabMtSlotSize(s), sizeof(Small));
  xSlabMtDestroy(s);
}

TEST(SlabMtTest, InvalidArgs) {
  EXPECT_EQ(xSlabMtCreate(0, 0, 0), nullptr);
  EXPECT_EQ(xSlabMtCreate(sizeof(Small), 3, 0), nullptr);
  xSlabMtDestroy(nullptr);
  xSlabMtFree(nullptr, nullptr);
  EXPECT_EQ(xSlabMtAlloc(nullptr), nullptr);
}

TEST(SlabMtTest, SingleThreadedRoundTrip) {
  xSlabMt *s = xSlabMtCreate(sizeof(Small), 16, 0);
  ASSERT_NE(s, nullptr);

  void *p = xSlabMtAlloc(s);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(is_aligned(p, 16));
  std::memset(p, 0xCD, sizeof(Small));
  xSlabMtFree(s, p);

  void *p2 = xSlabMtAlloc(s);
  EXPECT_EQ(p2, p);
  xSlabMtFree(s, p2);

  xSlabMtDestroy(s);
}

TEST(SlabMtTest, ConcurrentAllocFreeDistinct) {
  xSlabMt *s = xSlabMtCreate(sizeof(Large), 16, 4096);
  ASSERT_NE(s, nullptr);

  constexpr int kThreads   = 8;
  constexpr int kPerThread = 2000;

  std::atomic<bool>                go{false};
  std::vector<std::thread>         ts;
  std::vector<std::vector<void *>> out(kThreads);

  for (int t = 0; t < kThreads; t++) {
    ts.emplace_back([&, t] {
      while (!go.load(std::memory_order_acquire)) { /* spin */ }
      auto &local = out[t];
      local.reserve(kPerThread);
      for (int i = 0; i < kPerThread; i++) {
        void *p = xSlabMtAlloc(s);
        ASSERT_NE(p, nullptr);
        std::memset(p, (t * 131 + i) & 0xFF, sizeof(Large));
        local.push_back(p);
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (auto &t : ts) t.join();

  std::unordered_set<void *> all;
  for (auto &v : out) {
    for (void *p : v) {
      ASSERT_TRUE(all.insert(p).second) << "duplicate pointer";
    }
  }
  EXPECT_EQ(all.size(), (size_t)(kThreads * kPerThread));

  std::vector<std::thread> freers;
  for (int t = 0; t < kThreads; t++) {
    freers.emplace_back([&, t] {
      for (void *p : out[t]) xSlabMtFree(s, p);
    });
  }
  for (auto &t : freers) t.join();

  xSlabMtDestroy(s);
}

TEST(SlabMtTest, CrossThreadAllocFreeChurn) {
  xSlabMt *s = xSlabMtCreate(sizeof(Small), 16, 2048);
  ASSERT_NE(s, nullptr);

  constexpr int kThreads = 4;
  constexpr int kIters   = 20000;

  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; t++) {
    ts.emplace_back([&] {
      for (int i = 0; i < kIters; i++) {
        void *p = xSlabMtAlloc(s);
        ASSERT_NE(p, nullptr);
        *static_cast<volatile int *>(p) = i;
        xSlabMtFree(s, p);
      }
    });
  }
  for (auto &t : ts) t.join();
  xSlabMtDestroy(s);
}
