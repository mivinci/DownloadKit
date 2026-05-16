# HTTP/2 Server Benchmark

End-to-end HTTP/2 (h2c, cleartext) server benchmark comparing **moo** (single-threaded event-loop) against **Go** `net/http` + `x/net/http2/h2c` (goroutine-per-connection).

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| Load Generator | [h2load](https://nghttp2.org/documentation/h2load-howto.html) (nghttp2 1.68.1) — 4 threads, 10s duration, 10 max concurrent streams per connection |

## Server Implementations

### moo (`bench/http_bench_server.cpp`)

Single-threaded event-loop HTTP/2 server built on `xbase/event.h` + `xhttp/server.h`. Supports h2c (cleartext HTTP/2) via Prior Knowledge — the same binary as the HTTP/1.1 benchmark, since moo auto-detects the protocol on the first bytes of each connection.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DX_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./build/bench/http_bench_server 8080
```

### Go (`bench/h2c_bench_server.go`)

Standard `net/http` server wrapped with `golang.org/x/net/http2/h2c.NewHandler()` to support cleartext HTTP/2 via Prior Knowledge. Go's runtime spawns one goroutine per connection and uses its own epoll/kqueue poller internally.

```bash
cd bench && go build -o ../build/bench/go_h2c_bench h2c_bench_server.go
./build/bench/go_h2c_bench 8081
```

### Routes

Both servers implement identical routes:

| Route | Method | Description |
| --- | --- | --- |
| `/ping` | GET | Returns `"pong"` (4 bytes) — minimal response latency test |
| `/echo?size=N` | GET | Returns N bytes of `'x'` — variable response size test |
| `/echo` | POST | Echoes request body — request body throughput test |

## Benchmark Methodology

All benchmarks use [h2load](https://nghttp2.org/documentation/h2load-howto.html) with the following defaults unless noted:

- **4 threads** (`-t4`)
- **100 connections** (`-c100`)
- **10 max concurrent streams** per connection (`-m10`)
- **10 seconds** (`-D 10`)

POST benchmarks use `-d <file>` to specify the request body.

> **Why h2load?** Unlike wrk (HTTP/1.1 only), h2load is purpose-built for HTTP/2 benchmarking. It supports stream multiplexing (`-m`), h2c Prior Knowledge, and reports per-stream latency.

## Results

### GET /ping — Minimal Response Latency

Tests raw request/response overhead with a 4-byte `"pong"` response. Varies connection count to measure scalability under HTTP/2 multiplexing.

| Connections | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 50 | **576,249** | 141,655 | 863 μs | 3.51 ms | moo +307% |
| 100 | **561,825** | 120,732 | 1.78 ms | 8.27 ms | moo +365% |
| 200 | **555,800** | 110,143 | 3.59 ms | 18.10 ms | moo +405% |
| 500 | **538,905** | 136,719 | 9.22 ms | 36.21 ms | moo +294% |

**Analysis:**

- moo sustains **~560K req/s** across all connection counts — a massive improvement over its HTTP/1.1 numbers (~152K) thanks to HTTP/2 stream multiplexing on fewer TCP connections.
- Go's h2c throughput (~110–142K) is comparable to its HTTP/1.1 numbers, suggesting Go's HTTP/2 implementation doesn't benefit as much from multiplexing.
- moo's advantage ranges from **+294% to +405%** — far larger than the +18–24% gap seen in HTTP/1.1. The single-threaded event loop excels at handling multiplexed streams without context-switching overhead.
- At 200 connections, moo's advantage peaks at +405%. Go's throughput degrades more steeply under high connection counts due to goroutine scheduling and HTTP/2 flow control overhead.

### GET /echo — Variable Response Size

Tests response serialization throughput with different payload sizes under HTTP/2 framing. Fixed at 100 connections.

| Response Size | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 64 B | **518,176** | 123,386 | 1.92 ms | 8.08 ms | moo +320% |
| 256 B | **511,276** | 116,267 | 1.95 ms | 8.60 ms | moo +340% |
| 1 KiB | **493,405** | 115,267 | 2.03 ms | 8.64 ms | moo +328% |
| 4 KiB | **383,507** | 107,457 | 2.59 ms | 9.23 ms | moo +257% |

**Analysis:**

- moo throughput degrades gracefully from 518K to 384K req/s as response size grows from 64B to 4KB — a 26% drop, mostly due to HTTP/2 DATA frame serialization overhead.
- Go stays relatively flat (~107–123K) but at a much lower baseline. The `bytes.Repeat` allocation + GC pressure is compounded by HTTP/2 framing overhead.
- moo's advantage is consistently **+257% to +340%** — HTTP/2's HPACK header compression and binary framing amplify moo's architectural advantage over Go.

### POST /echo — Request Body Throughput

Tests request body parsing and echo throughput under HTTP/2. Fixed at 100 connections.

| Body Size | moo Req/s | Go Req/s | moo Transfer/s | Go Transfer/s | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 KiB | **401,047** | 119,739 | 399.45 MB/s | 119.82 MB/s | moo +235% |
| 4 KiB | **195,221** | 90,585 | 766.61 MB/s | 356.84 MB/s | moo +115% |
| 16 KiB | **57,304** | 41,313 | 896.83 MB/s | 648.24 MB/s | moo +39% |
| 64 KiB | **19,040** | 16,557 | 1.16 GB/s | 1.01 GB/s | moo +15% |

**Analysis:**

- moo achieves **1.16 GB/s** transfer rate at 64KB body size — comparable to its HTTP/1.1 performance (2.20 GB/s), with the difference attributable to HTTP/2 flow control and framing overhead.
- The advantage narrows from +235% (1KB) to +15% (64KB) as both servers become I/O bound. HTTP/2 flow control (default 64KB window) becomes the bottleneck at large payloads.
- At small payloads (1KB), moo's +235% advantage shows the efficiency of its nghttp2-based H2 implementation vs Go's `x/net/http2`.

## HTTP/2 vs HTTP/1.1 Comparison

How does HTTP/2 compare to HTTP/1.1 for each server? (GET /ping, 100 connections)

| Server | HTTP/1.1 Req/s | HTTP/2 Req/s | Δ |
| --- | ---: | ---: | --- |
| moo | 152,316 | 561,825 | **+269%** |
| Go | 128,915 | 120,732 | −6% |

**Key Insight:** moo's single-threaded event loop benefits enormously from HTTP/2 multiplexing — handling multiple streams on fewer connections eliminates per-connection overhead. Go's goroutine-per-connection model doesn't gain from multiplexing because it already handles concurrency at the goroutine level; the added HTTP/2 framing overhead actually causes a slight regression.

## Summary

```text
                    moo vs Go h2c (Release build, h2load -m10)
                    =============================================

  GET /ping:     moo +294% ~ +405%   (massive advantage across all concurrency)
  GET /echo:     moo +257% ~ +340%   (consistent across all response sizes)
  POST /echo:    moo +15%  ~ +235%   (advantage narrows as payloads grow)

  Peak throughput:  moo 576K req/s  (GET /ping, 50 connections)
  Peak transfer:    moo 1.16 GB/s   (POST /echo, 64KB body)
```

**Key Takeaways:**

1. **HTTP/2 amplifies moo's advantage.** The gap widens from +18–24% (HTTP/1.1) to +294–405% (HTTP/2) on GET /ping. Stream multiplexing plays to the strengths of a single-threaded event loop.
2. **moo scales with multiplexing.** moo's throughput jumps from 152K (HTTP/1.1) to 576K (HTTP/2) req/s — a 3.8× improvement. Go's throughput stays flat or slightly regresses.
3. **Payload efficiency.** At small-to-medium payloads, moo's nghttp2-based H2 implementation is dramatically faster. At large payloads (64KB), both servers converge as I/O and flow control dominate.
4. **Architecture matters even more for H2.** HTTP/2's stream multiplexing, HPACK compression, and flow control add complexity that a lean C event loop handles more efficiently than Go's runtime.

## Reproducing

```bash
# Build moo server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DX_BUILD_BENCHMARKS=ON
cmake --build build --parallel

# Build Go h2c server
cd bench && go build -o ../build/bench/go_h2c_bench h2c_bench_server.go && cd ..

# Install h2load (macOS)
brew install nghttp2

# Start servers
./build/bench/http_bench_server 8080 &
./build/bench/go_h2c_bench 8081 &

# GET /ping benchmark
h2load -t4 -c100 -m10 -D 10 http://127.0.0.1:8080/ping
h2load -t4 -c100 -m10 -D 10 http://127.0.0.1:8081/ping

# GET /echo benchmark
h2load -t4 -c100 -m10 -D 10 "http://127.0.0.1:8080/echo?size=1024"
h2load -t4 -c100 -m10 -D 10 "http://127.0.0.1:8081/echo?size=1024"

# POST /echo benchmark (create body file first)
dd if=/dev/zero bs=4096 count=1 | tr '\0' 'x' > /tmp/body_4k.bin
h2load -t4 -c100 -m10 -D 10 -d /tmp/body_4k.bin http://127.0.0.1:8080/echo
h2load -t4 -c100 -m10 -D 10 -d /tmp/body_4k.bin http://127.0.0.1:8081/echo

# Cleanup
pkill -f http_bench_server
pkill -f go_h2c_bench
```
