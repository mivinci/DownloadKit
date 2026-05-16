/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io_bench.cpp - Micro-benchmarks for xIOBuffer
 */

#include <benchmark/benchmark.h>

#include <vector>

extern "C" {
#include <x/buf/io.h>
}

// BM_IOBuffer_Append: Measure append throughput (block-chain allocation)
static void BM_IOBuffer_Append(benchmark::State &state) {
  const int64_t chunk_size = state.range(0);
  std::vector<char> data(chunk_size, 'x');

  for (auto _ : state) {
    xIOBuffer io;
    xIOBufferInit(&io);
    for (int i = 0; i < 1000; i++) {
      xIOBufferAppend(&io, data.data(), chunk_size);
    }
    xIOBufferDeinit(&io);
  }
  state.SetBytesProcessed(state.iterations() * 1000 * chunk_size);
}
BENCHMARK(BM_IOBuffer_Append)->Arg(64)->Arg(256)->Arg(1024)->Arg(4096)->Arg(8192);

// BM_IOBuffer_AppendConsume: Measure interleaved append + consume
static void BM_IOBuffer_AppendConsume(benchmark::State &state) {
  const int64_t chunk_size = state.range(0);
  std::vector<char> data(chunk_size, 'x');

  for (auto _ : state) {
    xIOBuffer io;
    xIOBufferInit(&io);
    for (int i = 0; i < 1000; i++) {
      xIOBufferAppend(&io, data.data(), chunk_size);
      xIOBufferConsume(&io, chunk_size / 2);
    }
    xIOBufferDeinit(&io);
  }
  state.SetBytesProcessed(state.iterations() * 1000 * chunk_size);
}
BENCHMARK(BM_IOBuffer_AppendConsume)->Arg(64)->Arg(256)->Arg(1024);

// BM_IOBuffer_Cut: Measure zero-copy cut throughput
static void BM_IOBuffer_Cut(benchmark::State &state) {
  const int64_t total = state.range(0);
  const int64_t cut_size = 1024;
  std::vector<char> data(total, 'x');

  for (auto _ : state) {
    xIOBuffer src;
    xIOBufferInit(&src);
    xIOBufferAppend(&src, data.data(), total);

    int64_t remaining = total;
    while (remaining >= cut_size) {
      xIOBuffer dst;
      xIOBufferInit(&dst);
      xIOBufferCut(&src, &dst, cut_size);
      remaining -= cut_size;
      xIOBufferDeinit(&dst);
    }
    xIOBufferDeinit(&src);
  }
  state.SetBytesProcessed(state.iterations() * total);
}
BENCHMARK(BM_IOBuffer_Cut)->Arg(8192)->Arg(65536)->Arg(262144);

// BM_IOBuffer_AppendIOBuffer: Measure zero-copy buffer concatenation
static void BM_IOBuffer_AppendIOBuffer(benchmark::State &state) {
  const int64_t chunk_size = state.range(0);
  std::vector<char> data(chunk_size, 'x');

  for (auto _ : state) {
    xIOBuffer dst;
    xIOBufferInit(&dst);

    for (int i = 0; i < 100; i++) {
      xIOBuffer src;
      xIOBufferInit(&src);
      xIOBufferAppend(&src, data.data(), chunk_size);
      xIOBufferAppendIOBuffer(&dst, &src);
      xIOBufferDeinit(&src);
    }

    xIOBufferDeinit(&dst);
  }
  state.SetBytesProcessed(state.iterations() * 100 * chunk_size);
}
BENCHMARK(BM_IOBuffer_AppendIOBuffer)->Arg(1024)->Arg(4096)->Arg(8192);

// BM_IOBuffer_BlockPool: Measure block acquire/release throughput
static void BM_IOBuffer_BlockPool(benchmark::State &state) {
  for (auto _ : state) {
    xIOBlock *blk = xIOBlockAcquire();
    benchmark::DoNotOptimize(blk);
    xIOBlockRelease(blk);
  }
}
BENCHMARK(BM_IOBuffer_BlockPool);
