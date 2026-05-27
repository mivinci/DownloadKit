/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * runtime_bench.cpp - Benchmarks for xpp::Runtime.
 *
 * Measures:
 *   - spawn + await overhead (single task round-trip)
 *   - throughput (many short tasks in parallel)
 *   - work stealing effectiveness under contention
 *   - lazy worker scaling behavior
 */

#include <xpp/compiler.h>

#if XPP_HAS_COROUTINES

#include <xpp/runtime.h>
#include <benchmark/benchmark.h>

#include <atomic>

/* ── Helpers ──────────────────────────────────────────────────────── */

static xpp::Promise<int> trivial_task() {
  co_return 1;
}

static xpp::Promise<int> yield_task() {
  co_await xpp::yield();
  co_return 1;
}

/* ── BM_BlockOn: baseline block_on overhead ───────────────────────── */

static void BM_BlockOn(benchmark::State &state) {
  xpp::Runtime rt;
  for (auto _ : state) {
    int v = rt.block_on([]() { return xpp::Promise<int>::resolve(42); });
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_BlockOn);

/* ── BM_SpawnOne: single spawn + co_await round-trip ──────────────── */

static void BM_SpawnOne(benchmark::State &state) {
  xpp::Runtime rt;
  for (auto _ : state) {
    int v = rt.block_on([&]() -> xpp::Promise<int> {
      auto h = rt.spawn(trivial_task);
      co_return co_await h;
    });
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_SpawnOne);

/* ── BM_SpawnYield: spawn a task that yields (forces event loop turn) */

static void BM_SpawnYield(benchmark::State &state) {
  xpp::Runtime rt;
  for (auto _ : state) {
    int v = rt.block_on([&]() -> xpp::Promise<int> {
      auto h = rt.spawn(yield_task);
      co_return co_await h;
    });
    benchmark::DoNotOptimize(v);
  }
}
BENCHMARK(BM_SpawnYield);

/* ── BM_SpawnMany: N tasks spawned and awaited ────────────────────── */

static void BM_SpawnMany(benchmark::State &state) {
  const int n = static_cast<int>(state.range(0));
  xpp::Runtime rt;

  for (auto _ : state) {
    int v = rt.block_on([&]() -> xpp::Promise<int> {
      int sum = 0;
      for (int i = 0; i < n; ++i) {
        auto h = rt.spawn(trivial_task);
        sum += co_await h;
      }
      co_return sum;
    });
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_SpawnMany)->Arg(10)->Arg(100)->Arg(1000);

/* ── BM_FanOut: spawn N tasks concurrently, then await all ────────── */

static void BM_FanOut(benchmark::State &state) {
  const int n = static_cast<int>(state.range(0));
  xpp::Runtime rt;

  for (auto _ : state) {
    int v = rt.block_on([&]() -> xpp::Promise<int> {
      // Spawn all tasks first (fan-out).
      xpp::JoinHandle<int> *handles = new xpp::JoinHandle<int>[n];
      for (int i = 0; i < n; ++i) {
        handles[i] = rt.spawn(trivial_task);
      }
      // Then await all (fan-in).
      int sum = 0;
      for (int i = 0; i < n; ++i) {
        sum += co_await handles[i];
      }
      delete[] handles;
      co_return sum;
    });
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_FanOut)->Arg(10)->Arg(100)->Arg(1000);

/* ── BM_Contention: all tasks fight for the same atomic ───────────── */

static void BM_Contention(benchmark::State &state) {
  const int n = static_cast<int>(state.range(0));
  xpp::Runtime rt;

  for (auto _ : state) {
    std::atomic<int> counter{0};
    auto work = [&]() -> xpp::Promise<void> {
      co_await xpp::yield();
      counter.fetch_add(1, std::memory_order_relaxed);
    };

    int v = rt.block_on([&]() -> xpp::Promise<int> {
      xpp::JoinHandle<void> *handles = new xpp::JoinHandle<void>[n];
      for (int i = 0; i < n; ++i) {
        handles[i] = rt.spawn(work);
      }
      for (int i = 0; i < n; ++i) {
        co_await handles[i];
      }
      delete[] handles;
      co_return counter.load();
    });
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_Contention)->Arg(100)->Arg(1000);

/* ── BM_WorkStealing: unbalanced load to exercise stealing ────────── */

static void BM_WorkStealing(benchmark::State &state) {
  const int n = static_cast<int>(state.range(0));
  xpp::Runtime rt;

  for (auto _ : state) {
    auto heavy = []() -> xpp::Promise<int> {
      // Simulate work with multiple yields.
      for (int i = 0; i < 10; ++i) {
        co_await xpp::yield();
      }
      co_return 1;
    };

    int v = rt.block_on([&]() -> xpp::Promise<int> {
      xpp::JoinHandle<int> *handles = new xpp::JoinHandle<int>[n];
      for (int i = 0; i < n; ++i) {
        handles[i] = rt.spawn(heavy);
      }
      int sum = 0;
      for (int i = 0; i < n; ++i) {
        sum += co_await handles[i];
      }
      delete[] handles;
      co_return sum;
    });
    benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
BENCHMARK(BM_WorkStealing)->Arg(100)->Arg(1000);

BENCHMARK_MAIN();

#else

// No coroutine support — empty benchmark.
#include <benchmark/benchmark.h>

static void BM_Disabled(benchmark::State &state) {
  for (auto _ : state) {}
}
BENCHMARK(BM_Disabled);
BENCHMARK_MAIN();

#endif // XPP_HAS_COROUTINES
