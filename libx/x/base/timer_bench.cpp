/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * timer_bench.cpp - Micro-benchmarks for xbase timer
 */

#include <benchmark/benchmark.h>

#include <atomic>
#include <vector>

extern "C" {
#include <x/base/timer.h>
}

// BM_Timer_SubmitCancel: Measure timer submit + cancel cycle (poll mode)
static void BM_Timer_SubmitCancel(benchmark::State &state) {
  xTimer t = xTimerCreate(nullptr); // Poll mode

  auto noop = [](void *) {};

  for (auto _ : state) {
    xTimerTask task = xTimerSubmitAfter(t, noop, nullptr, 1000000);
    xTimerCancel(t, task);
  }

  xTimerDestroy(t);
}
BENCHMARK(BM_Timer_SubmitCancel);

// BM_Timer_SubmitBatch: Measure batch submit throughput
static void BM_Timer_SubmitBatch(benchmark::State &state) {
  const int64_t n = state.range(0);
  xTimer        t = xTimerCreate(nullptr);

  auto                    noop = [](void *) {};
  std::vector<xTimerTask> tasks(n);

  for (auto _ : state) {
    for (int64_t i = 0; i < n; i++) {
      tasks[i] = xTimerSubmitAfter(t, noop, nullptr, 1000000 + i);
    }
    // Cancel all to clean up
    state.PauseTiming();
    for (int64_t i = 0; i < n; i++) {
      xTimerCancel(t, tasks[i]);
    }
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * n);

  xTimerDestroy(t);
}
BENCHMARK(BM_Timer_SubmitBatch)->Arg(10)->Arg(100)->Arg(1000);

// BM_Timer_FirePoll: Measure timer fire + poll throughput
static void BM_Timer_FirePoll(benchmark::State &state) {
  const int64_t n = state.range(0);
  xTimer        t = xTimerCreate(nullptr); // Poll mode

  std::atomic<int64_t> counter{0};
  auto                 cb = [](void *arg) {
    static_cast<std::atomic<int64_t> *>(arg)->fetch_add(1, std::memory_order_relaxed);
  };

  for (auto _ : state) {
    counter.store(0, std::memory_order_relaxed);
    // Submit timers that fire immediately (delay = 0)
    for (int64_t i = 0; i < n; i++) {
      xTimerSubmitAfter(t, cb, &counter, 0);
    }
    // Spin-poll until all timers have fired
    while (counter.load(std::memory_order_relaxed) < n) {
      xTimerPoll(t);
    }
  }
  state.SetItemsProcessed(state.iterations() * n);

  xTimerDestroy(t);
}
BENCHMARK(BM_Timer_FirePoll)->Arg(10)->Arg(100)->Arg(1000);
