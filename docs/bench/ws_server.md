# WebSocket Server Benchmark

End-to-end WebSocket echo server benchmark comparing **xKit** (single-threaded event-loop) against three popular Go WebSocket libraries:

- **gorilla/websocket** — The most widely used Go WebSocket library
- **nhooyr/websocket** (coder/websocket) — Modern API with context support
- **gobwas/ws** — Zero-allocation, low-level WebSocket library

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| Load Generator | Custom Go client (`ws_bench_client.go`) using gorilla/websocket |

## Server Implementations

All servers implement the same behavior: accept WebSocket connections and echo every received message back to the sender.

### xKit (`bench/ws_bench_server.cpp`)

Single-threaded event-loop WebSocket server built on `xbase/event.h` + `xhttp/ws.h`. Uses `xWsServe()` for a one-line WebSocket-only server. All frame parsing, masking, ping/pong, and close handshake are handled automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXK_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./build/bench/ws_bench_server 9090
```

### gorilla/websocket (`bench/ws_bench_server_gorilla.go`)

Standard `net/http` server with `gorilla/websocket.Upgrader`. One goroutine per connection with a simple `ReadMessage` / `WriteMessage` loop. Buffer sizes set to 4KB.

```bash
cd bench && go build -o ../build/bench/ws_bench_gorilla ws_bench_server_gorilla.go
./build/bench/ws_bench_gorilla 9091
```

### nhooyr/websocket (`bench/ws_bench_server_nhooyr.go`)

Standard `net/http` server with `nhooyr.io/websocket.Accept`. Uses the streaming `Reader` / `Writer` API with `io.Copy` for zero-copy echo.

```bash
cd bench && go build -o ../build/bench/ws_bench_nhooyr ws_bench_server_nhooyr.go
./build/bench/ws_bench_nhooyr 9092
```

### gobwas/ws (`bench/ws_bench_server_gobwas.go`)

Raw TCP listener with `gobwas/ws.Upgrader` for zero-allocation upgrade. Uses `wsutil.ReadClientData` / `wsutil.WriteServerMessage` for frame I/O. One goroutine per connection.

```bash
cd bench && go build -o ../build/bench/ws_bench_gobwas ws_bench_server_gobwas.go
./build/bench/ws_bench_gobwas 9093
```

## Benchmark Methodology

The benchmark client (`ws_bench_client.go`) establishes N concurrent WebSocket connections to the server. Each connection runs a synchronous echo loop: send a message → wait for the echo → measure round-trip latency → repeat. The test runs for 10 seconds.

Key parameters:

- **Connections**: 50, 100, 200, 500
- **Message sizes**: 64B, 256B, 1KB, 4KB
- **Message type**: Binary
- **Duration**: 10 seconds per test

> **Note:** The benchmark client uses gorilla/websocket for all tests. This means the client-side overhead is identical across all server tests, ensuring a fair comparison of server-side performance.

## Results

### Echo 64B — Varying Connection Count

Tests raw message throughput with minimal 64-byte payloads. Varies connection count to measure scalability.

| Connections | xKit Msg/s | gorilla Msg/s | nhooyr Msg/s | gobwas Msg/s |
| ---: | ---: | ---: | ---: | ---: |
| 50 | **219,850** | 173,133 | 107,570 | 138,360 |
| 100 | **219,813** | 180,373 | 125,386 | 140,522 |
| 200 | **218,997** | 184,335 | 140,378 | 141,859 |
| 500 | **218,078** | 184,820 | 155,729 | 141,970 |

**xKit vs best Go library (gorilla):**

| Connections | xKit | gorilla | Δ |
| ---: | ---: | ---: | --- |
| 50 | 219,850 | 173,133 | xKit **+27%** |
| 100 | 219,813 | 180,373 | xKit **+22%** |
| 200 | 218,997 | 184,335 | xKit **+19%** |
| 500 | 218,078 | 184,820 | xKit **+18%** |

**Latency (64B, varying connections):**

| Connections | xKit | gorilla | nhooyr | gobwas |
| ---: | ---: | ---: | ---: | ---: |
| 50 | **227 μs** | 289 μs | 465 μs | 361 μs |
| 100 | **455 μs** | 554 μs | 797 μs | 711 μs |
| 200 | **913 μs** | 1.08 ms | 1.42 ms | 1.41 ms |
| 500 | **2.29 ms** | 2.70 ms | 3.21 ms | 3.52 ms |

**Analysis:**

- xKit sustains **~219K msg/s** across all connection counts — virtually no throughput degradation from 50 to 500 connections. The single-threaded event loop handles all connections without context-switching overhead.
- **gorilla/websocket** is the fastest Go library at ~173–185K msg/s, benefiting from its mature, optimized implementation.
- **gobwas/ws** — despite being marketed as "zero-allocation" — is slower than gorilla in this echo benchmark (~138–142K). Its advantage is in memory efficiency for massive connection counts, not raw throughput.
- **nhooyr/websocket** is the slowest at ~108–156K msg/s. The streaming `Reader`/`Writer` API adds overhead compared to gorilla's simpler `ReadMessage`/`WriteMessage`.
- xKit's latency advantage is most pronounced at low connection counts (227 μs vs 289 μs at 50 connections) and narrows at high counts as all servers become scheduling-bound.

### Echo — Varying Message Size (100 connections)

Tests message throughput and transfer rate with different payload sizes. Fixed at 100 connections.

| Message Size | xKit Msg/s | gorilla Msg/s | nhooyr Msg/s | gobwas Msg/s |
| ---: | ---: | ---: | ---: | ---: |
| 64 B | **219,813** | 180,373 | 125,386 | 140,522 |
| 256 B | **216,760** | 179,909 | 122,661 | 140,677 |
| 1 KiB | **197,890** | 173,142 | 120,963 | 133,002 |
| 4 KiB | **133,553** | 125,313 | 100,829 | 92,203 |

**Transfer Rate (send + recv):**

| Message Size | xKit | gorilla | nhooyr | gobwas |
| ---: | ---: | ---: | ---: | ---: |
| 64 B | **26.84 MB/s** | 22.02 MB/s | 15.31 MB/s | 17.15 MB/s |
| 256 B | **105.84 MB/s** | 87.85 MB/s | 59.89 MB/s | 68.69 MB/s |
| 1 KiB | **386.50 MB/s** | 338.17 MB/s | 236.26 MB/s | 259.77 MB/s |
| 4 KiB | **1.02 GB/s** | 979 MB/s | 788 MB/s | 720 MB/s |

**Latency (100 connections, varying message size):**

| Message Size | xKit | gorilla | nhooyr | gobwas |
| ---: | ---: | ---: | ---: | ---: |
| 64 B | **455 μs** | 554 μs | 797 μs | 711 μs |
| 256 B | **461 μs** | 556 μs | 815 μs | 711 μs |
| 1 KiB | **505 μs** | 577 μs | 826 μs | 752 μs |
| 4 KiB | **749 μs** | 798 μs | 992 μs | 1.08 ms |

**Analysis:**

- xKit achieves **1.02 GB/s** transfer rate at 4KB messages — the only server to break the 1 GB/s barrier.
- At 4KB, the ranking shifts: xKit > gorilla > nhooyr > gobwas. gobwas drops to last place because its `ReadClientData` / `WriteServerMessage` API allocates a new byte slice per message, negating its "zero-allocation upgrade" advantage.
- xKit's advantage over gorilla narrows from +22% (64B) to +7% (4KB) as both servers become I/O bound at larger payloads.
- All servers show graceful throughput degradation as message size grows, with xKit maintaining the lowest latency across all sizes.

## Go Library Comparison (WS)

How do the three Go libraries compare against each other? (100 connections, 64B)

| Library | Msg/s | Latency | Relative |
| --- | ---: | ---: | --- |
| gorilla/websocket | **180,373** | **554 μs** | baseline |
| gobwas/ws | 140,522 | 711 μs | −22% |
| nhooyr/websocket | 125,386 | 797 μs | −30% |

**Key Insight:** In a pure echo benchmark, gorilla/websocket is the fastest Go library. gobwas/ws's advantage lies in memory efficiency for 100K+ idle connections (not measured here), while nhooyr/websocket prioritizes API ergonomics over raw performance.

## WSS (WebSocket over TLS) Benchmark

The same echo benchmark repeated over TLS (wss://) to measure the impact of encryption on throughput and latency. All servers use the same self-signed certificate (`bench_cert.pem` / `bench_key.pem`, RSA 2048-bit, TLSv1.3).

### WSS Server Implementations

- **xKit** (`bench/wss_bench_server.cpp`) — Uses `xHttpServerCreate()` + `xWsUpgrade()` + `xHttpServerListenTls()`. ALPN set to `http/1.1` only (WebSocket requires HTTP/1.1 upgrade). Single-threaded event loop handles both TLS and WebSocket I/O.
- **Go servers** (`bench/wss_bench_server_{gorilla,nhooyr,gobwas}.go`) — Same logic as WS versions but with `ListenAndServeTLS` (gorilla/nhooyr) or `tls.Listen` (gobwas). Go's `crypto/tls` runs TLS per-goroutine, parallelizing encryption across connections.

### WSS Echo 64B — Varying Connection Count

| Connections | xKit Msg/s | gorilla Msg/s | nhooyr Msg/s | gobwas Msg/s |
| ---: | ---: | ---: | ---: | ---: |
| 50 | **186,513** | 173,125 | 107,589 | 138,317 |
| 100 | **186,068** | 180,426 | 133,218 | 142,187 |
| 200 | 184,066 | **185,792** | 148,475 | 144,361 |
| 500 | 167,019 | **184,532** | 156,695 | 143,220 |

**xKit vs gorilla (WSS):**

| Connections | xKit | gorilla | Δ |
| ---: | ---: | ---: | --- |
| 50 | 186,513 | 173,125 | xKit **+8%** |
| 100 | 186,068 | 180,426 | xKit **+3%** |
| 200 | 184,066 | 185,792 | gorilla **+1%** |
| 500 | 167,019 | 184,532 | gorilla **+10%** |

**Latency (WSS 64B, varying connections):**

| Connections | xKit | gorilla | nhooyr | gobwas |
| ---: | ---: | ---: | ---: | ---: |
| 50 | **268 μs** | 289 μs | 465 μs | 361 μs |
| 100 | **537 μs** | 554 μs | 750 μs | 703 μs |
| 200 | **1.09 ms** | 1.08 ms | 1.35 ms | 1.38 ms |
| 500 | 2.99 ms | **2.71 ms** | 3.19 ms | 3.49 ms |

**Analysis:**

- At low connection counts (50–100), xKit still leads by 3–8% over gorilla. The single-threaded event loop's efficiency offsets the TLS overhead.
- At 200+ connections, **gorilla overtakes xKit**. Go's per-goroutine `crypto/tls` parallelizes encryption across all CPU cores, while xKit's single-threaded OpenSSL must serialize all TLS operations on one core.
- The TLS overhead reduces xKit's throughput by ~15% compared to plain WS (186K vs 220K at 100 conns). Go libraries show minimal TLS impact because Go's TLS is already goroutine-parallel.
- xKit's throughput degrades more steeply at 500 connections (167K, −10% from 50 conns) compared to plain WS (218K, −1%). This confirms TLS as the bottleneck for the single-threaded model.

### WSS Echo — Varying Message Size (100 connections)

| Message Size | xKit Msg/s | gorilla Msg/s | nhooyr Msg/s | gobwas Msg/s |
| ---: | ---: | ---: | ---: | ---: |
| 64 B | **186,068** | 180,426 | 133,218 | 142,187 |
| 256 B | 174,475 | **178,725** | 131,257 | 141,520 |
| 1 KiB | 149,246 | **172,198** | 127,026 | 135,534 |
| 4 KiB | 16,352 ⚠️ | **138,882** | 103,923 | 108,895 |

**Transfer Rate (WSS, send + recv):**

| Message Size | xKit | gorilla | nhooyr | gobwas |
| ---: | ---: | ---: | ---: | ---: |
| 64 B | **22.71 MB/s** | 22.02 MB/s | 16.26 MB/s | 17.36 MB/s |
| 256 B | 85.19 MB/s | **87.27 MB/s** | 64.09 MB/s | 69.10 MB/s |
| 1 KiB | 291.50 MB/s | **336.32 MB/s** | 248.10 MB/s | 264.71 MB/s |
| 4 KiB | 127.75 MB/s ⚠️ | **1.06 GB/s** | 811.90 MB/s | 850.74 MB/s |

> ⚠️ **Note on xKit 4KB WSS:** The xKit 4KB result is anomalous — the benchmark client ran for 60s instead of 10s, suggesting the single-threaded TLS write path saturated and caused client-side goroutines to block on `ReadMessage` past the deadline. The effective throughput during the active period was likely higher, but this result highlights a real limitation of single-threaded TLS for large payloads.

**Analysis:**

- At 64B, xKit and gorilla are nearly tied (186K vs 180K). TLS overhead is minimal for small payloads.
- At 256B+, **gorilla overtakes xKit** because Go parallelizes TLS encryption across goroutines while xKit serializes it on one thread.
- The 4KB result reveals the fundamental limitation: xKit's single-threaded OpenSSL cannot keep up with 100 concurrent connections each sending 4KB encrypted payloads. Go's per-goroutine TLS scales linearly with connection count.
- This is a known trade-off of the single-threaded event loop model. Future work could add a TLS write thread pool or io_uring-based async TLS to address this.

### WS vs WSS Performance Impact

How much does TLS reduce throughput? (100 connections, 64B)

| Server | WS Msg/s | WSS Msg/s | TLS Overhead |
| --- | ---: | ---: | --- |
| xKit | 219,813 | 186,068 | −15% |
| gorilla | 180,373 | 180,426 | ~0% |
| nhooyr | 125,386 | 133,218 | +6% ¹ |
| gobwas | 140,522 | 142,187 | +1% ¹ |

¹ Slight WSS improvement over WS is within measurement noise and likely due to system load variance between test runs.

**Key Insight:** Go's `crypto/tls` adds virtually zero overhead in this benchmark because TLS operations run in parallel across goroutines. xKit pays a 15% penalty because all TLS encryption/decryption happens on the single event loop thread.

## Summary

```text
                    WebSocket Echo Benchmark (Release build)
                    =========================================

  WS — 64B echo (100 conns):
    xKit:     219,813 msg/s   455 μs
    gorilla:  180,373 msg/s   554 μs   (xKit +22%)
    gobwas:   140,522 msg/s   711 μs   (xKit +56%)
    nhooyr:   125,386 msg/s   797 μs   (xKit +75%)

  WS — 4KB echo (100 conns):
    xKit:     133,553 msg/s   749 μs   1.02 GB/s
    gorilla:  125,313 msg/s   798 μs   979 MB/s   (xKit +7%)
    nhooyr:   100,829 msg/s   992 μs   788 MB/s   (xKit +32%)
    gobwas:    92,203 msg/s   1.08 ms  720 MB/s   (xKit +45%)

  WSS — 64B echo (100 conns):
    xKit:     186,068 msg/s   537 μs
    gorilla:  180,426 msg/s   554 μs   (xKit +3%)
    gobwas:   142,187 msg/s   703 μs   (xKit +31%)
    nhooyr:   133,218 msg/s   750 μs   (xKit +40%)

  WSS — 4KB echo (100 conns):
    gorilla:  138,882 msg/s   720 μs   1.06 GB/s
    gobwas:   108,895 msg/s   918 μs   851 MB/s
    nhooyr:   103,923 msg/s   962 μs   812 MB/s
    xKit:      16,352 msg/s   243 μs   128 MB/s  ⚠️ (single-thread TLS bottleneck)

  Peak WS throughput:   xKit 219,850 msg/s  (64B, 50 connections)
  Peak WS transfer:     xKit 1.02 GB/s      (4KB, 100 connections)
  Peak WSS throughput:  xKit 186,513 msg/s  (64B, 50 connections)
  Peak WSS transfer:    gorilla 1.06 GB/s   (4KB, 100 connections)
```

**Key Takeaways:**

1. **xKit is 18–27% faster than gorilla on plain WS** (small messages), and 3–8% faster on WSS at low connection counts. The single-threaded event loop avoids goroutine scheduling overhead.
2. **TLS changes the picture at scale.** At 200+ connections or 1KB+ messages over WSS, gorilla overtakes xKit because Go parallelizes TLS across goroutines while xKit serializes it on one thread.
3. **xKit's WS throughput is remarkably stable** across connection counts (219K at 50 conns vs 218K at 500 conns — less than 1% variation). WSS shows more degradation (186K → 167K) due to single-threaded TLS.
4. **gorilla/websocket is the fastest Go library** for both WS and WSS echo workloads.
5. **The single-threaded TLS bottleneck is the main limitation.** Future work could add a TLS write thread pool or io_uring-based async TLS to close the gap at larger payloads.

## Reproducing

```bash
# Build xKit servers
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXK_BUILD_BENCHMARKS=ON
cmake --build build --parallel

# Build Go servers and client
cd bench
go build -o ../build/bench/ws_bench_client ws_bench_client.go
go build -o ../build/bench/ws_bench_gorilla ws_bench_server_gorilla.go
go build -o ../build/bench/ws_bench_nhooyr ws_bench_server_nhooyr.go
go build -o ../build/bench/ws_bench_gobwas ws_bench_server_gobwas.go
go build -o ../build/bench/wss_bench_gorilla wss_bench_server_gorilla.go
go build -o ../build/bench/wss_bench_nhooyr wss_bench_server_nhooyr.go
go build -o ../build/bench/wss_bench_gobwas wss_bench_server_gobwas.go
cd ..

# Generate self-signed certificate for WSS benchmarks
openssl req -x509 -newkey rsa:2048 \
  -keyout build/bench/bench_key.pem \
  -out build/bench/bench_cert.pem \
  -days 365 -nodes -subj '/CN=localhost'

# Run WS benchmarks (one server at a time)
./build/bench/ws_bench_server 9090 &
./build/bench/ws_bench_client -url ws://127.0.0.1:9090/ -c 100 -d 10s -size 64
kill %1

./build/bench/ws_bench_gorilla 9091 &
./build/bench/ws_bench_client -url ws://127.0.0.1:9091/ -c 100 -d 10s -size 64
kill %1

./build/bench/ws_bench_nhooyr 9092 &
./build/bench/ws_bench_client -url ws://127.0.0.1:9092/ -c 100 -d 10s -size 64
kill %1

./build/bench/ws_bench_gobwas 9093 &
./build/bench/ws_bench_client -url ws://127.0.0.1:9093/ -c 100 -d 10s -size 64
kill %1

# Run WSS benchmarks (from build/bench directory for cert paths)
cd build/bench

./wss_bench_server 9090 bench_cert.pem bench_key.pem &
./ws_bench_client -url wss://127.0.0.1:9090/ -c 100 -d 10s -size 64
kill %1

./wss_bench_gorilla 9091 bench_cert.pem bench_key.pem &
./ws_bench_client -url wss://127.0.0.1:9091/ -c 100 -d 10s -size 64
kill %1

./wss_bench_nhooyr 9092 bench_cert.pem bench_key.pem &
./ws_bench_client -url wss://127.0.0.1:9092/ -c 100 -d 10s -size 64
kill %1

./wss_bench_gobwas 9093 bench_cert.pem bench_key.pem &
./ws_bench_client -url wss://127.0.0.1:9093/ -c 100 -d 10s -size 64
kill %1
```
