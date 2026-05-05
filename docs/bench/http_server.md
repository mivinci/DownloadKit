# HTTP Server Benchmark

End-to-end HTTP/1.1 server benchmark comparing **moo** (single-threaded event-loop) against **Go** `net/http` (goroutine-per-connection).

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| Load Generator | [wrk](https://github.com/wg/wrk) — 4 threads, 10s duration |

## Server Implementations

### moo (`bench/http_bench_server.cpp`)

Single-threaded event-loop HTTP/1.1 server built on `xbase/event.h` + `xhttp/server.h`. Uses kqueue on macOS, epoll on Linux. All I/O is handled in one thread — no thread pool, no goroutines.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOO_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./build/bench/http_bench_server 8080
```

### Go (`bench/http_bench_server.go`)

Standard `net/http` server with default settings. Go's runtime spawns one goroutine per connection and uses its own epoll/kqueue poller internally.

```bash
go build -o build/bench/go_http_bench bench/http_bench_server.go
./build/bench/go_http_bench 8081
```

### Routes

Both servers implement identical routes:

| Route | Method | Description |
| --- | --- | --- |
| `/ping` | GET | Returns `"pong"` (4 bytes) — minimal response latency test |
| `/echo?size=N` | GET | Returns N bytes of `'x'` — variable response size test |
| `/echo` | POST | Echoes request body — request body throughput test |

## Benchmark Methodology

All benchmarks use [wrk](https://github.com/wg/wrk) with the following defaults unless noted:

- **4 threads** (`-t4`)
- **100 connections** (`-c100`)
- **10 seconds** (`-d10s`)

POST benchmarks use Lua scripts to set the request body:

```lua
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/octet-stream"
wrk.body = string.rep("x", BODY_SIZE)
```

## Results

### GET /ping — Minimal Response Latency

Tests raw request/response overhead with a 4-byte `"pong"` response. Varies connection count to measure scalability.

| Connections | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 50 | **151,935** | 128,639 | 315 μs | 365 μs | moo +18% |
| 100 | **152,316** | 128,915 | 658 μs | 761 μs | moo +18% |
| 200 | **151,007** | 128,162 | 1.33 ms | 1.55 ms | moo +18% |
| 500 | **155,486** | 125,471 | 3.20 ms | 3.96 ms | moo +24% |

**Analysis:**

- moo maintains ~152K req/s regardless of connection count, showing excellent scalability of the single-threaded event loop.
- Go's throughput slightly degrades at 500 connections due to goroutine scheduling overhead.
- moo's advantage grows from +18% to +24% as connection count increases — the event loop's O(1) dispatch scales better than goroutine context switching.

### GET /echo — Variable Response Size

Tests response serialization throughput with different payload sizes. Fixed at 100 connections.

| Response Size | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 64 B | **150,592** | 127,432 | 666 μs | 771 μs | moo +18% |
| 256 B | **146,487** | 126,907 | 682 μs | 774 μs | moo +15% |
| 1 KiB | **144,831** | 125,729 | 689 μs | 785 μs | moo +15% |
| 4 KiB | **141,511** | 91,886 | 707 μs | 1.08 ms | moo +54% |

**Analysis:**

- moo throughput degrades gracefully from 151K to 142K req/s as response size grows from 64B to 4KB — only a 6% drop.
- Go drops sharply at 4KB (92K req/s, -27% from 64B), likely due to `bytes.Repeat` allocation pressure and GC overhead.
- moo's largest advantage (+54%) appears at 4KB, where Go's per-request heap allocation becomes the bottleneck.

### POST /echo — Request Body Throughput

Tests request body parsing and echo throughput. Fixed at 100 connections.

| Body Size | moo Req/s | Go Req/s | moo Transfer/s | Go Transfer/s | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 KiB | **141,495** | 122,584 | 152.35 MB/s | 133.51 MB/s | moo +15% |
| 4 KiB | **133,935** | 83,512 | 536.60 MB/s | 337.13 MB/s | moo +60% |
| 16 KiB | **82,231** | 53,828 | **1.26 GB/s** | 848.10 MB/s | moo +53% |
| 64 KiB | **35,908** | 31,124 | **2.20 GB/s** | 1.90 GB/s | moo +15% |

**Analysis:**

- moo achieves **2.20 GB/s** transfer rate at 64KB body size — impressive for a single-threaded server.
- The largest advantage (+60%) appears at 4KB, consistent with the GET /echo pattern — Go's allocation overhead dominates at medium payload sizes.
- At 64KB, the gap narrows to +15% as both servers become I/O bound (kernel socket buffer management dominates).

## Summary

```text
                    moo vs Go net/http (Release build)
                    ====================================

  GET /ping:     moo +18% ~ +24%   (consistent across all concurrency levels)
  GET /echo:     moo +15% ~ +54%   (advantage grows with response size)
  POST /echo:    moo +15% ~ +60%   (advantage peaks at medium body sizes)

  Peak throughput:  moo 155K req/s (GET /ping, 500 connections)
  Peak transfer:    moo 2.20 GB/s  (POST /echo, 64KB body)
```

**Key Takeaways:**

1. **moo wins every scenario.** A single-threaded C event loop outperforms Go's multi-goroutine runtime across all request types and payload sizes.
2. **Scalability.** moo's throughput is nearly flat from 50 to 500 connections. Go degrades under high connection counts due to goroutine scheduling overhead.
3. **Payload efficiency.** moo's advantage is most pronounced at medium payloads (1–4 KiB) where Go's per-request heap allocation and GC pressure become significant.
4. **Architecture matters.** moo's single-threaded design eliminates all synchronization overhead. Go pays for goroutine creation, scheduling, and garbage collection on every request.

## Reproducing

```bash
# Build moo server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOO_BUILD_BENCHMARKS=ON
cmake --build build --parallel

# Build Go server
go build -o build/bench/go_http_bench bench/http_bench_server.go

# Run moo benchmark
./build/bench/http_bench_server 8080 &
wrk -t4 -c100 -d10s http://127.0.0.1:8080/ping
wrk -t4 -c100 -d10s "http://127.0.0.1:8080/echo?size=64"
wrk -t4 -c100 -d10s "http://127.0.0.1:8080/echo?size=4096"

# POST with lua script
cat > /tmp/post.lua << 'EOF'
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/octet-stream"
wrk.body = string.rep("x", 4096)
EOF
wrk -t4 -c100 -d10s -s /tmp/post.lua http://127.0.0.1:8080/echo

# Run Go benchmark (same wrk commands, different port)
./build/bench/go_http_bench 8081 &
wrk -t4 -c100 -d10s http://127.0.0.1:8081/ping
```
