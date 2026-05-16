/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * task_bench.cpp - Micro-benchmarks for xbase task (thread pool)
 *
 * Measures the overhead of xTaskSubmit / xTaskWait under various
 * patterns: single-task round-trip, fan-out, concurrent submit, etc.
 */

#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>
#include <vector>

extern "C" {
#include <x/base/task.h>
}

#ifdef X_HAS_LIBUV
#include <uv.h>
#endif

/* ── Helpers ── */

static void *noop_fn(void *) {
  return nullptr;
}

static void *increment_fn(void *arg) {
  auto *counter = static_cast<std::atomic<int64_t> *>(arg);
  counter->fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

/* ═══════════════════════════════════════════════════════════════════
 *  BM_Task_SubmitWait: Single-task submit + wait round-trip latency.
 *
 *  This is the most common pattern in event-loop offload: submit one
 *  task, immediately wait for it.  Measures the full overhead of
 *  allocation, enqueue, worker dispatch, completion notification,
 *  and deallocation.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_Task_SubmitWait(benchmark::State &state) {
  xTaskGroupConf conf = {.nthreads = 4, .queue_cap = 0};
  xTaskGroup     g    = xTaskGroupCreate(&conf);

  for (auto _ : state) {
    xTask t = xTaskSubmit(g, noop_fn, nullptr);
    xTaskWait(t, nullptr);
  }

  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Task_SubmitWait);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_Task_FanOut: Submit N tasks, then GroupWait.
 *
 *  Measures throughput of batch submission + barrier synchronization.
 *  The range parameter controls the fan-out width (number of tasks).
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_Task_FanOut(benchmark::State &state) {
  const int64_t fan_out = state.range(0);

  xTaskGroupConf conf = {.nthreads = 4, .queue_cap = 0};
  xTaskGroup     g    = xTaskGroupCreate(&conf);

  std::atomic<int64_t> counter{0};

  for (auto _ : state) {
    counter.store(0, std::memory_order_relaxed);

    for (int64_t i = 0; i < fan_out; i++) {
      xTaskSubmit(g, increment_fn, &counter);
    }
    xTaskGroupWait(g);
  }

  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations() * fan_out);
}
BENCHMARK(BM_Task_FanOut)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_Task_SubmitWaitBatch: Submit N tasks, then wait each one.
 *
 *  Unlike FanOut (which uses GroupWait), this waits on each task
 *  individually.  Measures per-task wait overhead with TLS freelist
 *  recycling (submit and wait happen on the same thread).
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_Task_SubmitWaitBatch(benchmark::State &state) {
  const int64_t batch = state.range(0);

  xTaskGroupConf conf = {.nthreads = 4, .queue_cap = 0};
  xTaskGroup     g    = xTaskGroupCreate(&conf);

  std::vector<xTask> tasks(batch);

  for (auto _ : state) {
    for (int64_t i = 0; i < batch; i++) {
      tasks[i] = xTaskSubmit(g, noop_fn, nullptr);
    }
    for (int64_t i = 0; i < batch; i++) {
      xTaskWait(tasks[i], nullptr);
    }
  }

  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_Task_SubmitWaitBatch)->Arg(10)->Arg(100)->Arg(1000);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_Task_ConcurrentSubmit: Multiple threads submit to the same group.
 *
 *  Measures contention on the task queue lock under concurrent
 *  submission from N producer threads.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_Task_ConcurrentSubmit(benchmark::State &state) {
  const int         num_producers    = state.range(0);
  constexpr int64_t OPS_PER_PRODUCER = 1000;

  xTaskGroupConf conf = {.nthreads = 4, .queue_cap = 0};
  xTaskGroup     g    = xTaskGroupCreate(&conf);

  for (auto _ : state) {
    std::atomic<int>  ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    threads.reserve(num_producers);

    for (int t = 0; t < num_producers; t++) {
      threads.emplace_back([&]() {
        ready.fetch_add(1, std::memory_order_relaxed);
        while (!go.load(std::memory_order_acquire)) {}
        for (int64_t i = 0; i < OPS_PER_PRODUCER; i++) {
          xTaskSubmit(g, noop_fn, nullptr);
        }
      });
    }

    while (ready.load(std::memory_order_relaxed) < num_producers) {}
    go.store(true, std::memory_order_release);

    for (auto &th : threads)
      th.join();
    xTaskGroupWait(g);
  }

  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations() * num_producers * OPS_PER_PRODUCER);
}
BENCHMARK(BM_Task_ConcurrentSubmit)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_Task_WorkerScaling: Fixed workload, varying worker thread count.
 *
 *  Measures how throughput scales with the number of worker threads.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_Task_WorkerScaling(benchmark::State &state) {
  const size_t      nworkers = static_cast<size_t>(state.range(0));
  constexpr int64_t TASKS    = 10000;

  xTaskGroupConf conf = {.nthreads = nworkers, .queue_cap = 0};
  xTaskGroup     g    = xTaskGroupCreate(&conf);

  std::atomic<int64_t> counter{0};

  for (auto _ : state) {
    counter.store(0, std::memory_order_relaxed);
    for (int64_t i = 0; i < TASKS; i++) {
      xTaskSubmit(g, increment_fn, &counter);
    }
    xTaskGroupWait(g);
  }

  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations() * TASKS);
}
BENCHMARK(BM_Task_WorkerScaling)->Arg(1)->Arg(2)->Arg(4)->Arg(8);

/* ═══════════════════════════════════════════════════════════════════
 *  libuv baseline benchmarks (compiled only when X_HAS_LIBUV is set)
 *
 *  These mirror the xTask benchmarks above using libuv's uv_queue_work
 *  API for a fair apples-to-apples comparison.
 *
 *  Note: uv_queue_work requires a running event loop.  The after_work_cb
 *  fires on the loop thread, so we run uv_run() on a dedicated thread
 *  and use a semaphore / counter to synchronize.
 * ═══════════════════════════════════════════════════════════════════ */
#ifdef X_HAS_LIBUV

/* ── libuv helpers ── */

static void uv_noop_work(uv_work_t *) {}

/* ── BM_Libuv_SubmitWait ──
 *
 *  Single uv_queue_work + wait for after_work_cb.
 *  Comparable to BM_Task_SubmitWait.
 */
static void BM_Libuv_SubmitWait(benchmark::State &state) {
  uv_loop_t loop;
  uv_loop_init(&loop);

  for (auto _ : state) {
    uv_work_t         req;
    std::atomic<bool> done{false};

    req.data = &done;
    uv_queue_work(&loop, &req, uv_noop_work, [](uv_work_t *r, int) {
      auto *d = static_cast<std::atomic<bool> *>(r->data);
      d->store(true, std::memory_order_release);
    });
    /* Drive the loop until the after_work_cb fires. */
    while (!done.load(std::memory_order_acquire)) {
      uv_run(&loop, UV_RUN_ONCE);
    }
  }

  uv_loop_close(&loop);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Libuv_SubmitWait);

/* ── BM_Libuv_FanOut ──
 *
 *  Submit N tasks via uv_queue_work, then uv_run until all complete.
 *  Comparable to BM_Task_FanOut.
 */
static void BM_Libuv_FanOut(benchmark::State &state) {
  const int64_t fan_out = state.range(0);

  uv_loop_t loop;
  uv_loop_init(&loop);

  std::atomic<int64_t>   counter{0};
  std::vector<uv_work_t> reqs(fan_out);

  struct FanOutCtx {
    std::atomic<int64_t> *counter;
    std::atomic<int64_t> *remaining;
  };

  std::atomic<int64_t> remaining{0};
  FanOutCtx            ctx = {&counter, &remaining};

  for (auto _ : state) {
    counter.store(0, std::memory_order_relaxed);
    remaining.store(fan_out, std::memory_order_relaxed);

    for (int64_t i = 0; i < fan_out; i++) {
      reqs[i].data = &ctx;
      uv_queue_work(
        &loop, &reqs[i],
        [](uv_work_t *r) {
          auto *c = static_cast<FanOutCtx *>(r->data);
          c->counter->fetch_add(1, std::memory_order_relaxed);
        },
        [](uv_work_t *r, int) {
          auto *c = static_cast<FanOutCtx *>(r->data);
          c->remaining->fetch_sub(1, std::memory_order_release);
        });
    }

    while (remaining.load(std::memory_order_acquire) > 0) {
      uv_run(&loop, UV_RUN_ONCE);
    }
  }

  uv_loop_close(&loop);
  state.SetItemsProcessed(state.iterations() * fan_out);
}
BENCHMARK(BM_Libuv_FanOut)->Arg(10)->Arg(100)->Arg(1000)->Arg(10000);

/* ── BM_Libuv_SubmitWaitBatch ──
 *
 *  Submit N tasks, then run loop until all after_work_cb fire.
 *  Comparable to BM_Task_SubmitWaitBatch.
 */
static void BM_Libuv_SubmitWaitBatch(benchmark::State &state) {
  const int64_t batch = state.range(0);

  uv_loop_t loop;
  uv_loop_init(&loop);

  std::vector<uv_work_t> reqs(batch);
  std::atomic<int64_t>   remaining{0};

  for (auto _ : state) {
    remaining.store(batch, std::memory_order_relaxed);

    for (int64_t i = 0; i < batch; i++) {
      reqs[i].data = &remaining;
      uv_queue_work(&loop, &reqs[i], uv_noop_work, [](uv_work_t *r, int) {
        auto *rem = static_cast<std::atomic<int64_t> *>(r->data);
        rem->fetch_sub(1, std::memory_order_release);
      });
    }

    while (remaining.load(std::memory_order_acquire) > 0) {
      uv_run(&loop, UV_RUN_ONCE);
    }
  }

  uv_loop_close(&loop);
  state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_Libuv_SubmitWaitBatch)->Arg(10)->Arg(100)->Arg(1000);

#endif /* X_HAS_LIBUV */
