/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory_bench.cpp - Micro-benchmarks for xbase memory allocator
 */

#include <benchmark/benchmark.h>

#include <cstdlib>

extern "C" {
#include <x/base/memory.h>
}

namespace {

struct BenchObj {
  char data[64]; // Padding to simulate a real object
};

static void bench_obj_ctor(void *) {}
static void bench_obj_dtor(void *) {}

XDEF_VTABLE(BenchObj){
  bench_obj_ctor, bench_obj_dtor, nullptr, nullptr, nullptr, nullptr,
};

} // namespace

// BM_Memory_XAlloc: Measure xAlloc + xFree cycle
static void BM_Memory_XAlloc(benchmark::State &state) {
  const int64_t sz = state.range(0);
  for (auto _ : state) {
    void *p = xAlloc("BenchObj", sz, 1, &XSYM_VTABLE(BenchObj));
    benchmark::DoNotOptimize(p);
    xFree(p);
  }
}
BENCHMARK(BM_Memory_XAlloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// BM_Memory_Malloc: Baseline comparison with raw malloc/free
static void BM_Memory_Malloc(benchmark::State &state) {
  const int64_t sz = state.range(0);
  for (auto _ : state) {
    void *p = malloc(sz);
    benchmark::DoNotOptimize(p);
    free(p);
  }
}
BENCHMARK(BM_Memory_Malloc)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// BM_Memory_RetainRelease: Measure retain/release cycle overhead
static void BM_Memory_RetainRelease(benchmark::State &state) {
  void *p = xAlloc("BenchObj", sizeof(BenchObj), 1, &XSYM_VTABLE(BenchObj));
  for (auto _ : state) {
    xRetain(p);
    xRelease(p);
  }
  xFree(p);
}
BENCHMARK(BM_Memory_RetainRelease);
