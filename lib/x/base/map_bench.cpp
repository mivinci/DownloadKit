/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * map_bench.cpp - Micro-benchmarks for xbase map
 */

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <vector>

extern "C" {
#include <x/base/map.h>
}

namespace {

// Pre-generate integer keys as (void *) pointers
static std::vector<void *> make_keys(int64_t n) {
  std::vector<void *> keys(n);
  for (int64_t i = 0; i < n; i++) {
    keys[i] = reinterpret_cast<void *>(static_cast<uintptr_t>(i + 1));
  }
  return keys;
}

} // namespace

/* ═══════════════════════════════════════════════════════════════════
 *  Set
 * ═══════════════════════════════════════════════════════════════════ */

static void BM_Map_Set_Hash(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  for (auto _ : state) {
    xMap m = xMapCreate(xMapType_Hash, 16, xMapIntHash, xMapIntEq);
    for (int64_t i = 0; i < n; i++) {
      xMapSet(m, keys[i], keys[i]);
    }
    state.PauseTiming();
    xMapDestroy(m);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Set_Hash)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Set_Flat(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  for (auto _ : state) {
    xMap m = xMapCreate(xMapType_Flat, 16, xMapIntHash, xMapIntEq);
    for (int64_t i = 0; i < n; i++) {
      xMapSet(m, keys[i], keys[i]);
    }
    state.PauseTiming();
    xMapDestroy(m);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Set_Flat)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Set_Tree(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  for (auto _ : state) {
    xMap m = xMapCreate(xMapType_Tree, 0, xMapIntHash, xMapIntEq);
    for (int64_t i = 0; i < n; i++) {
      xMapSet(m, keys[i], keys[i]);
    }
    state.PauseTiming();
    xMapDestroy(m);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Set_Tree)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

/* ═══════════════════════════════════════════════════════════════════
 *  Get (hit)
 * ═══════════════════════════════════════════════════════════════════ */

static void BM_Map_Get_Hash(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  xMap m = xMapCreate(xMapType_Hash, 16, xMapIntHash, xMapIntEq);
  for (int64_t i = 0; i < n; i++) {
    xMapSet(m, keys[i], keys[i]);
  }

  for (auto _ : state) {
    for (int64_t i = 0; i < n; i++) {
      benchmark::DoNotOptimize(xMapGet(m, keys[i]));
    }
  }
  xMapDestroy(m);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Get_Hash)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Get_Flat(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  xMap m = xMapCreate(xMapType_Flat, 16, xMapIntHash, xMapIntEq);
  for (int64_t i = 0; i < n; i++) {
    xMapSet(m, keys[i], keys[i]);
  }

  for (auto _ : state) {
    for (int64_t i = 0; i < n; i++) {
      benchmark::DoNotOptimize(xMapGet(m, keys[i]));
    }
  }
  xMapDestroy(m);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Get_Flat)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Get_Tree(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  xMap m = xMapCreate(xMapType_Tree, 0, xMapIntHash, xMapIntEq);
  for (int64_t i = 0; i < n; i++) {
    xMapSet(m, keys[i], keys[i]);
  }

  for (auto _ : state) {
    for (int64_t i = 0; i < n; i++) {
      benchmark::DoNotOptimize(xMapGet(m, keys[i]));
    }
  }
  xMapDestroy(m);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Get_Tree)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

/* ═══════════════════════════════════════════════════════════════════
 *  Del
 * ═══════════════════════════════════════════════════════════════════ */

static void BM_Map_Del_Hash(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  for (auto _ : state) {
    state.PauseTiming();
    xMap m = xMapCreate(xMapType_Hash, 16, xMapIntHash, xMapIntEq);
    for (int64_t i = 0; i < n; i++) {
      xMapSet(m, keys[i], keys[i]);
    }
    state.ResumeTiming();

    for (int64_t i = 0; i < n; i++) {
      benchmark::DoNotOptimize(xMapDel(m, keys[i]));
    }

    state.PauseTiming();
    xMapDestroy(m);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Del_Hash)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Del_Flat(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  for (auto _ : state) {
    state.PauseTiming();
    xMap m = xMapCreate(xMapType_Flat, 16, xMapIntHash, xMapIntEq);
    for (int64_t i = 0; i < n; i++) {
      xMapSet(m, keys[i], keys[i]);
    }
    state.ResumeTiming();

    for (int64_t i = 0; i < n; i++) {
      benchmark::DoNotOptimize(xMapDel(m, keys[i]));
    }

    state.PauseTiming();
    xMapDestroy(m);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Del_Flat)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Del_Tree(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  for (auto _ : state) {
    state.PauseTiming();
    xMap m = xMapCreate(xMapType_Tree, 0, xMapIntHash, xMapIntEq);
    for (int64_t i = 0; i < n; i++) {
      xMapSet(m, keys[i], keys[i]);
    }
    state.ResumeTiming();

    for (int64_t i = 0; i < n; i++) {
      benchmark::DoNotOptimize(xMapDel(m, keys[i]));
    }

    state.PauseTiming();
    xMapDestroy(m);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Del_Tree)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

/* ═══════════════════════════════════════════════════════════════════
 *  Iterate
 * ═══════════════════════════════════════════════════════════════════ */

static bool iter_noop(const void *key, void *val, void *arg) {
  benchmark::DoNotOptimize(key);
  benchmark::DoNotOptimize(val);
  (void)arg;
  return true;
}

static void BM_Map_Iterate_Hash(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  xMap m = xMapCreate(xMapType_Hash, 16, xMapIntHash, xMapIntEq);
  for (int64_t i = 0; i < n; i++) {
    xMapSet(m, keys[i], keys[i]);
  }

  for (auto _ : state) {
    xMapIterate(m, iter_noop, nullptr);
  }
  xMapDestroy(m);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Iterate_Hash)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Iterate_Flat(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  xMap m = xMapCreate(xMapType_Flat, 16, xMapIntHash, xMapIntEq);
  for (int64_t i = 0; i < n; i++) {
    xMapSet(m, keys[i], keys[i]);
  }

  for (auto _ : state) {
    xMapIterate(m, iter_noop, nullptr);
  }
  xMapDestroy(m);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Iterate_Flat)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);

static void BM_Map_Iterate_Tree(benchmark::State &state) {
  const int64_t n    = state.range(0);
  auto          keys = make_keys(n);

  xMap m = xMapCreate(xMapType_Tree, 0, xMapIntHash, xMapIntEq);
  for (int64_t i = 0; i < n; i++) {
    xMapSet(m, keys[i], keys[i]);
  }

  for (auto _ : state) {
    xMapIterate(m, iter_noop, nullptr);
  }
  xMapDestroy(m);
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Map_Iterate_Tree)->Arg(64)->Arg(512)->Arg(4096)->Arg(32768);
