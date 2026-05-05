/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ring_bench.cpp - Micro-benchmarks for xRingBuffer
 */

#include <benchmark/benchmark.h>

#include <vector>

extern "C" {
#include <xbuf/ring.h>
}

// BM_Ring_WriteRead: Measure write + read throughput
static void BM_Ring_WriteRead(benchmark::State &state) {
  const int64_t chunk_size = state.range(0);
  std::vector<char> data(chunk_size, 'x');
  std::vector<char> out(chunk_size);

  // Ring capacity must be >= chunk_size
  xRingBuffer rb = xRingBufferCreate(chunk_size * 4);

  for (auto _ : state) {
    xRingBufferWrite(rb, data.data(), chunk_size);
    xRingBufferRead(rb, out.data(), chunk_size);
  }
  state.SetBytesProcessed(state.iterations() * chunk_size * 2);

  xRingBufferDestroy(rb);
}
BENCHMARK(BM_Ring_WriteRead)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// BM_Ring_Throughput: Measure sustained write throughput until full
static void BM_Ring_Throughput(benchmark::State &state) {
  const int64_t cap = state.range(0);
  const int64_t chunk = 64;
  std::vector<char> data(chunk, 'x');

  for (auto _ : state) {
    xRingBuffer rb = xRingBufferCreate(cap);
    int64_t written = 0;
    while (xRingBufferWrite(rb, data.data(), chunk) > 0) {
      written += chunk;
    }
    benchmark::DoNotOptimize(written);
    xRingBufferDestroy(rb);
  }
  state.SetBytesProcessed(state.iterations() * cap);
}
BENCHMARK(BM_Ring_Throughput)->Arg(4096)->Arg(16384)->Arg(65536);
