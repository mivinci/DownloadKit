# WSS Async TLS Offload — Performance Regression Report

This document records the benchmark results after introducing **async TLS offload** (BIO pair + thread pool) to the OpenSSL backend, compared against the previous **synchronous TLS** baseline from [ws_server.md](ws_server.md).

## Changes Under Test

The following changes were applied to the OpenSSL TLS transport:

1. **Async TLS offload**: TLS encryption/decryption is offloaded from the event loop thread to a worker thread pool via `xEventLoopSubmit`. The event loop thread handles socket I/O and BIO data transfer, while worker threads perform `SSL_read`/`SSL_write`.
2. **BIO pair transport**: Replaced direct `SSL_read(fd)`/`SSL_write(fd)` with a BIO pair architecture: `read(fd)` → `BIO_write(bio_net)` → worker `SSL_read` → `BIO_read(bio_int)` → callback.
3. **xRingBuffer replaces xMemBIO\_**: In `transport_mbedtls.c`, the custom `xMemBIO_` ring buffer was replaced with the shared `xRingBuffer` from `xbuf/`.
4. **xRingBufferWrite semantic change**: `xRingBufferWrite` changed from all-or-nothing (`xErrno`) to partial-write (`size_t`), merging the old `xRingBufferWritePartial`.

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| TLS Backend | OpenSSL (system) |
| Certificate | RSA 2048-bit, self-signed, TLSv1.3 |
| Load Generator | `ws_bench_client.go` (gorilla/websocket) |

## Results

### WSS Echo 64B — Varying Connection Count

| Connections | Sync TLS (baseline) | Async TLS Offload | Δ Throughput | Δ Latency |
| ---: | ---: | ---: | --- | --- |
| 50 | 186,513 msg/s, 268 μs | 56,737 msg/s, 881 μs | **−70%** | **+229%** |
| 100 | 186,068 msg/s, 537 μs | 56,692 msg/s, 1.76 ms | **−70%** | **+228%** |
| 200 | 184,066 msg/s, 1.09 ms | 57,223 msg/s, 3.49 ms | **−69%** | **+220%** |
| 500 | 167,019 msg/s, 2.99 ms | 55,144 msg/s, 9.06 ms | **−67%** | **+203%** |

### WSS Echo — Varying Message Size (100 connections)

| Message Size | Sync TLS (baseline) | Async TLS Offload | Δ Throughput |
| ---: | ---: | ---: | --- |
| 64 B | 165,952 msg/s | 56,692 msg/s | **−66%** |
| 256 B | 174,475 msg/s | 54,170 msg/s | **−69%** |
| 1 KiB | 149,246 msg/s | 54,589 msg/s | **−63%** |
| 4 KiB | 92,686 msg/s | 51,142 msg/s | **−45%** |

### Transfer Rate (100 connections)

| Message Size | Sync TLS | Async TLS Offload | Δ |
| ---: | ---: | ---: | --- |
| 64 B | 20.26 MB/s | 6.92 MB/s | −66% |
| 256 B | 85.19 MB/s | 26.45 MB/s | −69% |
| 1 KiB | 291.50 MB/s | 106.62 MB/s | −63% |
| 4 KiB | 723.95 MB/s | 399.55 MB/s | −45% |

### Latency (100 connections, varying message size)

| Message Size | Sync TLS | Async TLS Offload | Δ |
| ---: | ---: | ---: | --- |
| 64 B | 537 μs | 1.76 ms | +228% |
| 256 B | — | 1.85 ms | — |
| 1 KiB | — | 1.83 ms | — |
| 4 KiB | — | 1.95 ms | — |

## Analysis

### Performance is severely degraded

Across all test cases, the async TLS offload shows a **65–70% throughput reduction** and **2–3× latency increase** compared to the synchronous TLS baseline. The degradation is consistent across connection counts and message sizes.

### Root causes

1. **Thread pool scheduling overhead dominates small-message TLS cost.** For 64-byte messages, AES-GCM encryption/decryption takes on the order of nanoseconds, but each `xEventLoopSubmit` → worker thread → done callback round-trip costs tens of microseconds due to context switching, mutex contention, and cache invalidation. The scheduling overhead is orders of magnitude larger than the crypto work itself.

2. **Extra data copies through BIO pair.** The synchronous path does `SSL_read(fd)` directly — one syscall, zero copies between buffers. The async path requires: `read(fd)` → memcpy into `xRingBuffer(inbound)` → worker thread `SSL_read` reads from BIO → `BIO_write` output → memcpy into `xRingBuffer(outbound)` → `write(fd)`. This adds at least 2 extra memcpy operations per message direction.

3. **Serialization bottleneck not eliminated.** The async offload was intended to free the event loop thread from TLS work, but the event loop still must: (a) `read(fd)` ciphertext, (b) feed it into the inbound ring buffer, (c) drain the outbound ring buffer, (d) `write(fd)` ciphertext. The worker thread only does the SSL state machine. For a single-threaded event loop, this splits one thread's work into two threads' **serial** work (event loop → worker → event loop), adding synchronization overhead without parallelism.

4. **Throughput ceiling around 57K msg/s.** The async path's throughput is remarkably stable across connection counts (55K–57K), suggesting the bottleneck is the per-message offload overhead rather than I/O or crypto. This is consistent with a fixed per-message cost of ~17 μs (1/57K), which matches typical thread pool dispatch latency.

5. **4KB messages show the smallest regression (−45%).** As message size grows, the crypto cost increases relative to the fixed scheduling overhead, making the offload less wasteful. This confirms that the overhead is per-message, not per-byte.

### Comparison with Go goroutine-parallel TLS

For reference, gorilla/websocket achieves ~180K msg/s on WSS with virtually zero TLS overhead compared to plain WS. Go's `crypto/tls` runs per-goroutine, parallelizing encryption across all CPU cores without the BIO-pair indirection. This is the model that async TLS offload was trying to approximate, but the single event loop + thread pool architecture cannot match it.

## Conclusion

The async TLS offload architecture is a **net negative** for the WSS echo workload. The per-message thread dispatch overhead far exceeds the TLS crypto cost for small-to-medium messages (64B–4KB).

### Recommendations

1. **Revert to synchronous TLS for the default path.** The synchronous `SSL_read(fd)`/`SSL_write(fd)` model is 3× faster for this workload. The event loop thread can handle TLS inline without issue.

2. **Consider async offload only for large payloads.** If async TLS is desired, gate it behind a message-size threshold (e.g., >16KB) where the crypto cost justifies the dispatch overhead.

3. **Explore multi-threaded event loops instead.** Rather than offloading TLS from a single event loop, run multiple event loop threads (one per core), each handling its own connections with synchronous TLS. This is how Go achieves parallelism — not by offloading crypto, but by running independent I/O loops in parallel.

4. **If async TLS is kept, optimize the dispatch path.** Reduce per-message overhead by batching multiple SSL operations per dispatch, using lock-free queues, or coalescing small messages before offloading.
