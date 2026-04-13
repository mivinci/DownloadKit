/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_bench.cpp - Micro-benchmarks for xbase event loop
 *
 * Measures event loop overhead across several dimensions:
 *   - Create/Destroy lifecycle
 *   - Cross-thread wake latency
 *   - I/O source add/del cycle
 *   - Timer scheduling (single + batch)
 *   - Offload round-trip (submit work → done callback on loop thread)
 *
 * When XK_HAS_LIBUV is defined, libuv baselines are included for
 * wake latency, timer, and offload benchmarks.
 */

#include <benchmark/benchmark.h>

#include <atomic>
#include <unistd.h>
#include <vector>

extern "C" {
#include <xbase/event.h>
}

#ifdef XK_HAS_LIBUV
#include <uv.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 *  xKit event loop benchmarks
 * ═══════════════════════════════════════════════════════════════════ */

// BM_EventLoop_CreateDestroy: Measure event loop creation/destruction overhead
static void BM_EventLoop_CreateDestroy(benchmark::State &state) {
  for (auto _ : state) {
    xEventLoop loop = xEventLoopCreate();
    benchmark::DoNotOptimize(loop);
    xEventLoopDestroy(loop);
  }
}
BENCHMARK(BM_EventLoop_CreateDestroy);

// BM_EventLoop_WakeLatency: Measure cross-thread wake latency
static void BM_EventLoop_WakeLatency(benchmark::State &state) {
  xEventLoop loop = xEventLoopCreate();

  for (auto _ : state) {
    xEventWake(loop);
    xEventWait(loop, 0);
  }

  xEventLoopDestroy(loop);
}
BENCHMARK(BM_EventLoop_WakeLatency);

// BM_EventLoop_PipeAddDel: Measure add/del cycle with a real fd (pipe)
static void BM_EventLoop_PipeAddDel(benchmark::State &state) {
  xEventLoop loop = xEventLoopCreate();
  int        fds[2];
  pipe(fds);

  auto noop = [](int, xEventMask, void *) {};

  for (auto _ : state) {
    xEventSource src = xEventAdd(loop, fds[0], xEvent_Read, noop, nullptr);
    xEventDel(loop, src);
  }

  close(fds[0]);
  close(fds[1]);
  xEventLoopDestroy(loop);
}
BENCHMARK(BM_EventLoop_PipeAddDel);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_EventLoop_TimerSingle: Single 0ms timer round-trip.
 *
 *  Register a timer with delay=0, then xEventWait to fire it.
 *  Measures heap push + pop + dispatch overhead per timer.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_EventLoop_TimerSingle(benchmark::State &state) {
  xEventLoop loop = xEventLoopCreate();

  for (auto _ : state) {
    std::atomic<bool> fired{false};
    xEventLoopTimerAfter(
      loop,
      [](void *arg) {
        static_cast<std::atomic<bool> *>(arg)->store(true,
                                                     std::memory_order_release);
      },
      &fired, 0);
    xEventWait(loop, 0);
  }

  xEventLoopDestroy(loop);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventLoop_TimerSingle);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_EventLoop_TimerBatch: Register N timers (delay=0), fire all in
 *  one xEventWait call.
 *
 *  Measures timer heap throughput under bulk insertion + extraction.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_EventLoop_TimerBatch(benchmark::State &state) {
  const int64_t batch = state.range(0);
  xEventLoop    loop  = xEventLoopCreate();

  auto noop_timer = [](void *) {};

  for (auto _ : state) {
    for (int64_t i = 0; i < batch; i++) {
      xEventLoopTimerAfter(loop, noop_timer, nullptr, 0);
    }
    xEventWait(loop, 0);
  }

  xEventLoopDestroy(loop);
  state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_EventLoop_TimerBatch)->Arg(10)->Arg(100)->Arg(1000);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_EventLoop_OffloadSingle: Single submit + done_fn round-trip.
 *
 *  Submit a noop work function, wait for done_fn to fire on the loop
 *  thread.  Measures the full offload path: task submit → worker
 *  dispatch → MPSC push → wake → drain → done callback.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_EventLoop_OffloadSingle(benchmark::State &state) {
  xTaskGroupConf conf = {.nthreads = 4, .queue_cap = 0};
  xTaskGroup     g    = xTaskGroupCreate(&conf);
  xEventLoop     loop = xEventLoopCreateWithGroup(g);

  for (auto _ : state) {
    std::atomic<bool> done{false};

    xEventLoopSubmit(
      loop, nullptr,
      [](void *) -> void * { return nullptr; },
      [](void *arg, void *) {
        static_cast<std::atomic<bool> *>(arg)->store(
          true, std::memory_order_release);
      },
      &done);

    /* Drive the loop until done_fn fires. */
    while (!done.load(std::memory_order_acquire)) {
      xEventWait(loop, 10);
    }
  }

  xEventLoopDestroy(loop);
  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EventLoop_OffloadSingle);

/* ═══════════════════════════════════════════════════════════════════
 *  BM_EventLoop_OffloadBatch: Submit N work items, wait for all
 *  done_fn callbacks.
 *
 *  Measures offload throughput with batched submissions.
 * ═══════════════════════════════════════════════════════════════════ */
static void BM_EventLoop_OffloadBatch(benchmark::State &state) {
  const int64_t  batch = state.range(0);
  xTaskGroupConf conf  = {.nthreads = 4, .queue_cap = 0};
  xTaskGroup     g     = xTaskGroupCreate(&conf);
  xEventLoop     loop  = xEventLoopCreateWithGroup(g);

  for (auto _ : state) {
    std::atomic<int64_t> remaining{batch};

    for (int64_t i = 0; i < batch; i++) {
      xEventLoopSubmit(
        loop, nullptr,
        [](void *) -> void * { return nullptr; },
        [](void *arg, void *) {
          static_cast<std::atomic<int64_t> *>(arg)->fetch_sub(
            1, std::memory_order_release);
        },
        &remaining);
    }

    while (remaining.load(std::memory_order_acquire) > 0) {
      xEventWait(loop, 10);
    }
  }

  xEventLoopDestroy(loop);
  xTaskGroupDestroy(g);
  state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_EventLoop_OffloadBatch)->Arg(10)->Arg(100)->Arg(1000);

/* ═══════════════════════════════════════════════════════════════════
 *  libuv baseline benchmarks (compiled only when XK_HAS_LIBUV is set)
 * ═══════════════════════════════════════════════════════════════════ */
#ifdef XK_HAS_LIBUV

/* ── BM_Libuv_WakeLatency ──
 *
 *  uv_async_send + uv_run(UV_RUN_ONCE).
 *  Comparable to BM_EventLoop_WakeLatency.
 */
static void BM_Libuv_WakeLatency(benchmark::State &state) {
  uv_loop_t loop;
  uv_loop_init(&loop);

  uv_async_t async;
  uv_async_init(&loop, &async, [](uv_async_t *) {});

  for (auto _ : state) {
    uv_async_send(&async);
    uv_run(&loop, UV_RUN_ONCE);
  }

  uv_close(reinterpret_cast<uv_handle_t *>(&async), nullptr);
  uv_run(&loop, UV_RUN_DEFAULT); /* drain close callback */
  uv_loop_close(&loop);
}
BENCHMARK(BM_Libuv_WakeLatency);

/* ── BM_Libuv_TimerSingle ──
 *
 *  Single 0ms timer via uv_timer_start + uv_run.
 *  Comparable to BM_EventLoop_TimerSingle.
 */
static void BM_Libuv_TimerSingle(benchmark::State &state) {
  uv_loop_t loop;
  uv_loop_init(&loop);

  uv_timer_t timer;
  uv_timer_init(&loop, &timer);

  for (auto _ : state) {
    uv_timer_start(&timer, [](uv_timer_t *) {}, 0, 0);
    uv_run(&loop, UV_RUN_ONCE);
  }

  uv_close(reinterpret_cast<uv_handle_t *>(&timer), nullptr);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Libuv_TimerSingle);

/* ── BM_Libuv_TimerBatch ──
 *
 *  Register N timers (timeout=0), run loop until all fire.
 *  Comparable to BM_EventLoop_TimerBatch.
 *
 *  Note: libuv timers are heap-allocated handles that must be closed,
 *  so we pre-allocate a vector and reuse across iterations.
 */
static void BM_Libuv_TimerBatch(benchmark::State &state) {
  const int64_t batch = state.range(0);

  uv_loop_t loop;
  uv_loop_init(&loop);

  std::vector<uv_timer_t> timers(batch);
  for (int64_t i = 0; i < batch; i++) {
    uv_timer_init(&loop, &timers[i]);
  }

  for (auto _ : state) {
    std::atomic<int64_t> remaining{batch};

    for (int64_t i = 0; i < batch; i++) {
      timers[i].data = &remaining;
      uv_timer_start(
        &timers[i],
        [](uv_timer_t *t) {
          auto *rem = static_cast<std::atomic<int64_t> *>(t->data);
          rem->fetch_sub(1, std::memory_order_release);
        },
        0, 0);
    }

    while (remaining.load(std::memory_order_acquire) > 0) {
      uv_run(&loop, UV_RUN_ONCE);
    }
  }

  /* Close all timer handles */
  for (int64_t i = 0; i < batch; i++) {
    uv_close(reinterpret_cast<uv_handle_t *>(&timers[i]), nullptr);
  }
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
  state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_Libuv_TimerBatch)->Arg(10)->Arg(100)->Arg(1000);

/* ── BM_Libuv_OffloadSingle ──
 *
 *  Single uv_queue_work + wait for after_work_cb.
 *  Comparable to BM_EventLoop_OffloadSingle.
 */
static void BM_Libuv_OffloadSingle(benchmark::State &state) {
  uv_loop_t loop;
  uv_loop_init(&loop);

  for (auto _ : state) {
    uv_work_t          req;
    std::atomic<bool>  done{false};

    req.data = &done;
    uv_queue_work(
      &loop, &req, [](uv_work_t *) {},
      [](uv_work_t *r, int) {
        auto *d = static_cast<std::atomic<bool> *>(r->data);
        d->store(true, std::memory_order_release);
      });

    while (!done.load(std::memory_order_acquire)) {
      uv_run(&loop, UV_RUN_ONCE);
    }
  }

  uv_loop_close(&loop);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Libuv_OffloadSingle);

/* ── BM_Libuv_OffloadBatch ──
 *
 *  Submit N work items via uv_queue_work, run loop until all complete.
 *  Comparable to BM_EventLoop_OffloadBatch.
 */
static void BM_Libuv_OffloadBatch(benchmark::State &state) {
  const int64_t batch = state.range(0);

  uv_loop_t loop;
  uv_loop_init(&loop);

  std::vector<uv_work_t> reqs(batch);

  for (auto _ : state) {
    std::atomic<int64_t> remaining{batch};

    for (int64_t i = 0; i < batch; i++) {
      reqs[i].data = &remaining;
      uv_queue_work(
        &loop, &reqs[i], [](uv_work_t *) {},
        [](uv_work_t *r, int) {
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
BENCHMARK(BM_Libuv_OffloadBatch)->Arg(10)->Arg(100)->Arg(1000);

#endif /* XK_HAS_LIBUV */
