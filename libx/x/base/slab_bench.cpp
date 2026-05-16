/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slab_bench.cpp - xSlab / xSlabMt micro-benchmarks
 *
 * Compares xSlab's fixed-size pool against raw malloc/free and
 * calloc/free for the allocation patterns that dominate xbase:
 * many small objects allocated and freed at high frequency.
 */

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <x/base/slab.h>
}

namespace {
struct Obj64 {
  char pad[64];
};
} // namespace

/* ───────── Hot-path alloc+free cycle ───────── */

static void BM_Slab_AllocFree(benchmark::State &state) {
  xSlab *s = xSlabCreate(sizeof(Obj64), 16, 0);
  for (auto _ : state) {
    void *p = xSlabAlloc(s);
    benchmark::DoNotOptimize(p);
    xSlabFree(s, p);
  }
  xSlabDestroy(s);
}
BENCHMARK(BM_Slab_AllocFree);

static void BM_Malloc_AllocFree(benchmark::State &state) {
  for (auto _ : state) {
    void *p = malloc(sizeof(Obj64));
    benchmark::DoNotOptimize(p);
    free(p);
  }
}
BENCHMARK(BM_Malloc_AllocFree);

static void BM_Calloc_AllocFree(benchmark::State &state) {
  for (auto _ : state) {
    void *p = calloc(1, sizeof(Obj64));
    benchmark::DoNotOptimize(p);
    free(p);
  }
}
BENCHMARK(BM_Calloc_AllocFree);

/* ───────── Batch alloc then batch free (more realistic) ───────── */

static void BM_Slab_Batch(benchmark::State &state) {
  const int64_t      n = state.range(0);
  xSlab             *s = xSlabCreate(sizeof(Obj64), 16, 0);
  std::vector<void *> ptrs(n);
  for (auto _ : state) {
    for (int64_t i = 0; i < n; i++) ptrs[i] = xSlabAlloc(s);
    for (int64_t i = 0; i < n; i++) xSlabFree(s, ptrs[i]);
  }
  xSlabDestroy(s);
}
BENCHMARK(BM_Slab_Batch)->Arg(16)->Arg(256)->Arg(4096);

static void BM_Malloc_Batch(benchmark::State &state) {
  const int64_t      n = state.range(0);
  std::vector<void *> ptrs(n);
  for (auto _ : state) {
    for (int64_t i = 0; i < n; i++) ptrs[i] = malloc(sizeof(Obj64));
    for (int64_t i = 0; i < n; i++) free(ptrs[i]);
  }
}
BENCHMARK(BM_Malloc_Batch)->Arg(16)->Arg(256)->Arg(4096);

/* ───────── Multi-threaded xSlabMt ───────── */

static void BM_SlabMt_AllocFree(benchmark::State &state) {
  static xSlabMt *s = nullptr;
  if (state.thread_index() == 0) {
    s = xSlabMtCreate(sizeof(Obj64), 16, 0);
  }
  for (auto _ : state) {
    void *p = xSlabMtAlloc(s);
    benchmark::DoNotOptimize(p);
    xSlabMtFree(s, p);
  }
  if (state.thread_index() == 0) {
    xSlabMtDestroy(s);
    s = nullptr;
  }
}
BENCHMARK(BM_SlabMt_AllocFree)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_MallocMt_AllocFree(benchmark::State &state) {
  for (auto _ : state) {
    void *p = malloc(sizeof(Obj64));
    benchmark::DoNotOptimize(p);
    free(p);
  }
}
BENCHMARK(BM_MallocMt_AllocFree)->Threads(1)->Threads(2)->Threads(4)->Threads(8);
