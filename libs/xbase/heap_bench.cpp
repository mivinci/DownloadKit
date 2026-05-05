/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * heap_bench.cpp - Micro-benchmarks for xbase heap
 */

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <vector>

extern "C" {
#include <xbase/heap.h>
}

namespace {

struct HeapElem {
  int    value;
  size_t heap_idx;
};

static int heap_cmp(const void *a, const void *b) {
  int va = static_cast<const HeapElem *>(a)->value;
  int vb = static_cast<const HeapElem *>(b)->value;
  return (va > vb) - (va < vb);
}

static void heap_setidx(void *e, size_t idx) {
  static_cast<HeapElem *>(e)->heap_idx = idx;
}

} // namespace

// BM_Heap_Push: Measure push throughput at different heap sizes
static void BM_Heap_Push(benchmark::State &state) {
  const int64_t n = state.range(0);
  std::vector<HeapElem> elems(n);

  for (auto _ : state) {
    state.PauseTiming();
    xHeap h = xHeapCreate(heap_cmp, heap_setidx, n);
    for (int64_t i = 0; i < n; i++) {
      elems[i].value = rand();
    }
    state.ResumeTiming();

    for (int64_t i = 0; i < n; i++) {
      xHeapPush(h, &elems[i]);
    }

    state.PauseTiming();
    xHeapDestroy(h);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Heap_Push)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// BM_Heap_Pop: Measure pop throughput at different heap sizes
static void BM_Heap_Pop(benchmark::State &state) {
  const int64_t n = state.range(0);
  std::vector<HeapElem> elems(n);

  for (auto _ : state) {
    state.PauseTiming();
    xHeap h = xHeapCreate(heap_cmp, heap_setidx, n);
    for (int64_t i = 0; i < n; i++) {
      elems[i].value = rand();
      xHeapPush(h, &elems[i]);
    }
    state.ResumeTiming();

    for (int64_t i = 0; i < n; i++) {
      xHeapPop(h);
    }

    state.PauseTiming();
    xHeapDestroy(h);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Heap_Pop)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);

// BM_Heap_Remove: Measure random removal throughput at different heap sizes
static void BM_Heap_Remove(benchmark::State &state) {
  const int64_t n = state.range(0);
  std::vector<HeapElem> elems(n);

  for (auto _ : state) {
    state.PauseTiming();
    xHeap h = xHeapCreate(heap_cmp, heap_setidx, n);
    for (int64_t i = 0; i < n; i++) {
      elems[i].value = rand();
      xHeapPush(h, &elems[i]);
    }
    // Build a random removal order
    std::vector<size_t> order(n);
    for (int64_t i = 0; i < n; i++) order[i] = i;
    for (int64_t i = n - 1; i > 0; i--) {
      size_t j = rand() % (i + 1);
      std::swap(order[i], order[j]);
    }
    state.ResumeTiming();

    for (int64_t i = 0; i < n; i++) {
      xHeapRemove(h, elems[order[i]].heap_idx);
    }

    state.PauseTiming();
    xHeapDestroy(h);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Heap_Remove)->Arg(8)->Arg(64)->Arg(512)->Arg(4096);
