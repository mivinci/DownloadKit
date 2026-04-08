<!-- markdownlint-disable MD033 -->
<!-- markdownlint-disable MD041 -->
<p align="center">
  <img src="docs/logo.png" alt="xKit" height="160">
</p>

<div align="center">
  <a href="https://le0.me/xKit">Docs</a>
  <span>&nbsp;&nbsp;•&nbsp;&nbsp;</span>
  <a href="diary">Diary</a>
  <span>&nbsp;&nbsp;•&nbsp;&nbsp;</span>
  <a href="STYLE.md">Style</a>
  <br />
  <br />
</div>

A collection of low-level C building blocks for event-driven, asynchronous programming on **macOS** and **Linux** (Windows is on the roadmap but not a near-term priority).

- Designed and reviewed by Leo X.
- Coded by Codebuddy VSCode plugin with claude-4.6-opus

## Modules

| Module | Description |
| ------ | ----------- |
| **[xbase](https://le0.me/xKit/xbase)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](https://le0.me/xKit/xbuf)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xnet](https://le0.me/xKit/xnet)** | Networking primitives — URL parser, async DNS resolver, shared TLS configuration types |
| **[xhttp](https://le0.me/xKit/xhttp)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, HTTP/1.1 (llhttp) & HTTP/2 (nghttp2) async server with TLS (OpenSSL / MbedTLS) and parameterized routing, WebSocket server & client (RFC 6455) |
| **[xlog](https://le0.me/xKit/xlog)** | Async logging — MPSC queue, timer/pipe flush, log rotation |

📖 See the **[full documentation](https://le0.me/xKit)** for detailed design, architecture diagrams, API references, and usage examples.

## Prerequisites

| Dependency | Required | Notes |
| ------------ | ---------- | ------- |
| CMake ≥ 3.14 | ✅ | Build system |
| C99 compiler | ✅ | GCC or Clang |
| GoogleTest | For tests | `libgtest-dev` (apt) / `googletest` (brew) |
| libcurl | ✅ | Enables the **xhttp** client |
| llhttp | ✅ | HTTP/1.1 parsing for **xhttp** server — `libllhttp-dev` (apt) / `llhttp` (brew) |
| nghttp2 | ✅ | HTTP/2 support for **xhttp** server — `libnghttp2-dev` (apt) / `nghttp2` (brew) |
| OpenSSL | ✅ pick one | TLS backend for **xhttp** — `libssl-dev` (apt) / `openssl` (brew) |
| MbedTLS | ✅ pick one | TLS backend for **xhttp** — `libmbedtls-dev` (apt) / `mbedtls` (brew) |
| libunwind | Optional | Better backtraces on Linux |

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

To skip tests:

```bash
cmake -S . -B build -DXK_BUILD_TESTS=OFF
```

## Test

### Local (macOS / Linux)

```bash
ctest --test-dir build --output-on-failure --parallel 4
```

### TLS backend selection

The **xhttp** module supports two TLS backends. Use `XK_TLS_BACKEND` to choose one at configure time:

| Backend | Value | Extra dependency |
| ------- | ----- | ---------------- |
| OpenSSL | `openssl` | `libssl-dev` (apt) / `openssl` (brew) |
| MbedTLS | `mbedtls` | `libmbedtls-dev` (apt) / `mbedtls` (brew) |

**OpenSSL** (default when available):

```bash
cmake -S . -B build-openssl -DCMAKE_BUILD_TYPE=Debug -DXK_TLS_BACKEND=openssl
cmake --build build-openssl --parallel
ctest --test-dir build-openssl --output-on-failure --parallel 4
```

**MbedTLS**:

```bash
cmake -S . -B build-mbedtls -DCMAKE_BUILD_TYPE=Debug -DXK_TLS_BACKEND=mbedtls
cmake --build build-mbedtls --parallel
ctest --test-dir build-mbedtls --output-on-failure --parallel 4
```

> **Tip:** To test both backends in one go, simply run the above commands sequentially with separate build directories.

### HTTPS integration tests

The `xhttp/https_test.cpp` suite tests the client and server TLS integration end-to-end. It generates self-signed certificates at test time (requires `openssl` CLI) and covers:

- HTTPS GET / POST / Do with `skip_verify`
- Custom CA path verification
- Self-signed certificate rejection (verify enabled)
- Wrong CA path failure
- Mutual TLS (mTLS) with client certificates
- mTLS failure when client cert is missing
- Concurrent HTTPS requests
- Request timeout over HTTPS
- TLS config reset between requests
- Destroy client with in-flight HTTPS request

These tests run automatically with `ctest` when `XK_TLS_BACKEND=openssl` is set.

### Linux via container (macOS host)

Requires macOS 26+ with [Apple Containerization](https://developer.apple.com/documentation/containerization):

```bash
brew install container
container system start
./scripts/test-linux.sh            # default: gcc:14, Debug, -j2
./scripts/test-linux.sh -j4 -m 4G  # custom parallelism and memory
```

## Benchmark

### Test Environment

| Item | Value |
| ---- | ----- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| L1d Cache | 64 KiB per core |
| L2 Cache | 4 MiB per core |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |

### Micro-Benchmark Results

Build and run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXK_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./scripts/run_micro_bench.sh
```

#### xbase — MPSC Queue

| Benchmark | Time | Throughput |
| --------- | ---- | ---------- |
| SingleProducer (1024 batch) | 10.9 μs | **94.4 M ops/s** |
| MultiProducer / 2 threads | 926 μs | 67.2 M ops/s |
| MultiProducer / 4 threads | 1.7 ms | 69.8 M ops/s |
| MultiProducer / 8 threads | 5.3 ms | 68.4 M ops/s |

#### xbase — Event Loop

| Benchmark | Time |
| --------- | ---- |
| Create / Destroy | 2.7 μs |
| Wake Latency | 854 ns |
| Pipe Add / Del | 1.2 μs |

#### xbase — Timer

| Benchmark | 10 timers | 100 timers | 1000 timers |
| --------- | --------- | ---------- | ----------- |
| Submit Batch | 1.9 μs (5.3 M/s) | 12.6 μs (8.0 M/s) | 126 μs (8.0 M/s) |
| Fire & Poll | 3.5 μs (2.9 M/s) | 17.7 μs (5.7 M/s) | 140 μs (7.2 M/s) |
| Submit / Cancel (single) | 117 ns | — | — |

#### xbase — Heap

| Benchmark | 8 | 64 | 512 | 4096 |
| --------- | - | -- | --- | ---- |
| Push | 1.1 μs (7.2 M/s) | 2.6 μs (24.7 M/s) | 15.2 μs (33.8 M/s) | 117 μs (35.0 M/s) |
| Pop | 1.2 μs (6.7 M/s) | 5.3 μs (12.2 M/s) | 59 μs (8.7 M/s) | 677 μs (6.1 M/s) |
| Remove | 1.3 μs (6.0 M/s) | 3.3 μs (19.7 M/s) | 20.7 μs (24.7 M/s) | 169 μs (24.3 M/s) |

#### xbase — Memory

| Benchmark | 16 B | 64 B | 256 B | 1 KiB | 4 KiB |
| --------- | ---- | ---- | ----- | ----- | ----- |
| xAlloc + xRelease | 28.0 ns | 26.6 ns | 25.8 ns | 25.3 ns | 29.3 ns |
| malloc + free | 17.3 ns | 20.2 ns | 20.5 ns | 24.3 ns | 17.3 ns |
| Retain / Release | 4.78 ns | — | — | — | — |

#### xbuf — Buffer

| Benchmark | 16 B | 64 B | 256 B | 1 KiB | 4 KiB |
| --------- | ---- | ---- | ----- | ----- | ----- |
| Append | 1.6 GiB/s | 6.8 GiB/s | 20.8 GiB/s | 37.1 GiB/s | 40.4 GiB/s |
| Append + Consume | — | 5.5 GiB/s | 17.5 GiB/s | 35.3 GiB/s | — |

#### xbuf — RingBuffer

| Benchmark | 64 B | 256 B | 1 KiB | 4 KiB |
| --------- | ---- | ----- | ----- | ----- |
| Write + Read | 7.6 GiB/s | 22.2 GiB/s | 50.9 GiB/s | **73.3 GiB/s** |

| Throughput (bulk) | 4 KiB | 16 KiB | 64 KiB |
| ----------------- | ----- | ------ | ------ |
| Write + Read | 7.6 GiB/s | 8.2 GiB/s | 8.3 GiB/s |

#### xbuf — IOBuffer

| Benchmark | 64 B | 256 B | 1 KiB | 4 KiB | 8 KiB |
| --------- | ---- | ----- | ----- | ----- | ----- |
| Append | 6.6 GiB/s | 19.5 GiB/s | 35.9 GiB/s | 44.8 GiB/s | 40.7 GiB/s |
| Append + Consume | 3.7 GiB/s | 12.5 GiB/s | 27.1 GiB/s | — | — |
| Cut (zero-copy) | — | — | — | — | 26.1 GiB/s |
| Append IOBuffer | — | — | 18.2 GiB/s | 37.5 GiB/s | 42.6 GiB/s |
| Block Pool (acquire/release) | 10.2 ns | — | — | — | — |

### End-to-End HTTP Server Benchmark

**xKit** single-threaded event-loop HTTP/1.1 server (`bench/http_bench_server.cpp`) vs **Go** `net/http` multi-goroutine server (`bench/http_bench_server.go`), tested with [wrk](https://github.com/wg/wrk) (4 threads, 10 s duration, 100 connections unless noted).

```bash
# xKit
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXK_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./build/bench/http_bench_server 8080

# Go
go build -o build/bench/go_http_bench bench/http_bench_server.go
./build/bench/go_http_bench 8081

# Benchmark
wrk -t4 -c100 -d10s http://127.0.0.1:8080/ping
```

#### GET /ping (4-byte "pong" response)

| Connections | xKit Req/s | Go Req/s | xKit Latency | Go Latency | Δ |
| ----------- | ---------- | -------- | ------------ | ---------- | - |
| 50 | **151,935** | 128,639 | 315 μs | 365 μs | xKit +18% |
| 100 | **152,316** | 128,915 | 658 μs | 761 μs | xKit +18% |
| 200 | **151,007** | 128,162 | 1.33 ms | 1.55 ms | xKit +18% |
| 500 | **155,486** | 125,471 | 3.20 ms | 3.96 ms | xKit +24% |

#### GET /echo (variable response size)

| Response Size | xKit Req/s | Go Req/s | xKit Latency | Go Latency | Δ |
| ------------- | ---------- | -------- | ------------ | ---------- | - |
| 64 B | **150,592** | 127,432 | 666 μs | 771 μs | xKit +18% |
| 256 B | **146,487** | 126,907 | 682 μs | 774 μs | xKit +15% |
| 1 KiB | **144,831** | 125,729 | 689 μs | 785 μs | xKit +15% |
| 4 KiB | **141,511** | 91,886 | 707 μs | 1.08 ms | xKit +54% |

#### POST /echo (echo request body)

| Body Size | xKit Req/s | Go Req/s | xKit Transfer/s | Go Transfer/s | Δ |
| --------- | ---------- | -------- | --------------- | ------------- | - |
| 1 KiB | **141,495** | 122,584 | 152.35 MB/s | 133.51 MB/s | xKit +15% |
| 4 KiB | **133,935** | 83,512 | 536.60 MB/s | 337.13 MB/s | xKit +60% |
| 16 KiB | **82,231** | 53,828 | **1.26 GB/s** | 848.10 MB/s | xKit +53% |
| 64 KiB | **35,908** | 31,124 | **2.20 GB/s** | 1.90 GB/s | xKit +15% |

> **Notes:**
>
> - xKit uses a single-threaded event loop; Go `net/http` uses goroutine-per-connection with the full Go runtime.
> - Both built with full optimizations (xKit: `-O2` Release; Go: default compiler optimizations).
> - xKit dominates across **all** scenarios — GET /ping, GET /echo (all sizes), and POST /echo.
> - The largest advantage appears in medium-to-large payloads (GET 4 KiB: +54%, POST 4 KiB: +60%).

## License

[MIT](LICENSE) © 2025-present Leo X. and xKit contributors
