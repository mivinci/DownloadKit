/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_bench.cpp - Micro-benchmarks for xBuffer
 */

#include <benchmark/benchmark.h>

#include <vector>

extern "C" {
#include <x/buf/buf.h>
}

// BM_Buffer_Append: Measure append throughput at different chunk sizes
static void BM_Buffer_Append(benchmark::State &state) {
  const int64_t chunk_size = state.range(0);
  std::vector<char> data(chunk_size, 'x');

  for (auto _ : state) {
    xBuffer buf = xBufferCreate(1024);
    for (int i = 0; i < 1000; i++) {
      xBufferAppend(&buf, data.data(), chunk_size);
    }
    xBufferDestroy(buf);
  }
  state.SetBytesProcessed(state.iterations() * 1000 * chunk_size);
}
BENCHMARK(BM_Buffer_Append)->Arg(16)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096);

// BM_Buffer_AppendConsume: Measure interleaved append + consume
static void BM_Buffer_AppendConsume(benchmark::State &state) {
  const int64_t chunk_size = state.range(0);
  std::vector<char> data(chunk_size, 'x');

  for (auto _ : state) {
    xBuffer buf = xBufferCreate(chunk_size * 4);
    for (int i = 0; i < 1000; i++) {
      xBufferAppend(&buf, data.data(), chunk_size);
      xBufferConsume(buf, chunk_size / 2);
    }
    xBufferDestroy(buf);
  }
  state.SetBytesProcessed(state.iterations() * 1000 * chunk_size);
}
BENCHMARK(BM_Buffer_AppendConsume)->Arg(64)->Arg(256)->Arg(1024);
