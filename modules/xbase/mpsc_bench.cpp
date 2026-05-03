/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc_bench.cpp - Micro-benchmarks for xbase MPSC queue
 */

#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <xbase/mpsc.h>
}

namespace {

struct MpscNode {
  xMpsc link;
};

} // namespace

// BM_Mpsc_SingleProducer: Single-thread push/pop throughput
static void BM_Mpsc_SingleProducer(benchmark::State &state) {
  constexpr int64_t     BATCH = 1024;
  std::vector<MpscNode> nodes(BATCH);

  for (auto _ : state) {
    xMpsc *head = nullptr;
    xMpsc *tail = nullptr;

    for (int64_t i = 0; i < BATCH; i++) {
      nodes[i].link.next = nullptr;
      xMpscPush(&head, &tail, &nodes[i].link);
    }

    for (int64_t i = 0; i < BATCH; i++) {
      xMpscPop(&head, &tail);
    }
  }
  state.SetItemsProcessed(state.iterations() * BATCH);
}
BENCHMARK(BM_Mpsc_SingleProducer);

// BM_Mpsc_MultiProducer: Multi-thread push, single-thread pop
static void BM_Mpsc_MultiProducer(benchmark::State &state) {
  const int         num_threads    = state.range(0);
  constexpr int64_t OPS_PER_THREAD = 10000;

  for (auto _ : state) {
    xMpsc            *head = nullptr;
    xMpsc            *tail = nullptr;
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};

    // Allocate nodes for all threads
    std::vector<std::vector<MpscNode>> thread_nodes(num_threads);
    for (int t = 0; t < num_threads; t++) {
      thread_nodes[t].resize(OPS_PER_THREAD);
    }

    // Spawn producer threads
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; t++) {
      threads.emplace_back([&, t]() {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!go.load(std::memory_order_acquire)) {}
        for (int64_t i = 0; i < OPS_PER_THREAD; i++) {
          thread_nodes[t][i].link.next = nullptr;
          xMpscPush(&head, &tail, &thread_nodes[t][i].link);
        }
      });
    }

    // Wait for all threads to be ready, then signal go
    while (ready.load(std::memory_order_relaxed) < num_threads) {}
    go.store(true, std::memory_order_release);

    // Wait for all producers to finish
    for (auto &th : threads)
      th.join();

    // Consumer: pop all
    int64_t popped = 0;
    while (xMpscPop(&head, &tail) != nullptr) {
      popped++;
    }
    benchmark::DoNotOptimize(popped);
  }
  state.SetItemsProcessed(state.iterations() * num_threads * OPS_PER_THREAD);
}
BENCHMARK(BM_Mpsc_MultiProducer)->Arg(2)->Arg(4)->Arg(8);
