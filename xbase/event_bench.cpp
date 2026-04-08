/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_bench.cpp - Micro-benchmarks for xbase event loop
 */

#include <benchmark/benchmark.h>

#include <unistd.h>

extern "C" {
#include <xbase/event.h>
}

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
  int fds[2];
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
