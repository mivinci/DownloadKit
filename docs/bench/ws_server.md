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

## Go Library Comparison

How do the three Go libraries compare against each other? (100 connections, 64B)

| Library | Msg/s | Latency | Relative |
| --- | ---: | ---: | --- |
| gorilla/websocket | **180,373** | **554 μs** | baseline |
| gobwas/ws | 140,522 | 711 μs | −22% |
| nhooyr/websocket | 125,386 | 797 μs | −30% |

**Key Insight:** In a pure echo benchmark, gorilla/websocket is the fastest Go library. gobwas/ws's advantage lies in memory efficiency for 100K+ idle connections (not measured here), while nhooyr/websocket prioritizes API ergonomics over raw performance.

## Summary

```text
                    WebSocket Echo Benchmark (Release build)
                    =========================================

  64B echo (100 conns):
    xKit:     219,813 msg/s   455 μs
    gorilla:  180,373 msg/s   554 μs   (xKit +22%)
    gobwas:   140,522 msg/s   711 μs   (xKit +56%)
    nhooyr:   125,386 msg/s   797 μs   (xKit +75%)

  4KB echo (100 conns):
    xKit:     133,553 msg/s   749 μs   1.02 GB/s
    gorilla:  125,313 msg/s   798 μs   979 MB/s   (xKit +7%)
    nhooyr:   100,829 msg/s   992 μs   788 MB/s   (xKit +32%)
    gobwas:    92,203 msg/s   1.08 ms  720 MB/s   (xKit +45%)

  Peak throughput:  xKit 219,850 msg/s  (64B, 50 connections)
  Peak transfer:    xKit 1.02 GB/s      (4KB, 100 connections)
```

**Key Takeaways:**

1. **xKit is 18–27% faster than the best Go library (gorilla/websocket)** on small messages, and 7% faster on 4KB messages. The single-threaded event loop avoids goroutine scheduling overhead.
2. **xKit's throughput is remarkably stable** across connection counts (219K at 50 conns vs 218K at 500 conns — less than 1% variation). Go libraries show more variance due to goroutine scheduling.
3. **gorilla/websocket is the fastest Go library** for echo workloads, beating both gobwas/ws (−22%) and nhooyr/websocket (−30%) at 64B messages.
4. **gobwas/ws underperforms in echo benchmarks** despite its "zero-allocation" marketing. Its strength is memory efficiency for massive idle connection pools, not per-message throughput.
5. **The gap narrows at larger payloads** as all servers become I/O bound. At 4KB, xKit's advantage over gorilla is only +7%.

## Reproducing

```bash
# Build xKit server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXK_BUILD_BENCHMARKS=ON
cmake --build build --parallel

# Build Go servers and client
cd bench
go build -o ../build/bench/ws_bench_client ws_bench_client.go
go build -o ../build/bench/ws_bench_gorilla ws_bench_server_gorilla.go
go build -o ../build/bench/ws_bench_nhooyr ws_bench_server_nhooyr.go
go build -o ../build/bench/ws_bench_gobwas ws_bench_server_gobwas.go
cd ..

# Run benchmarks (one server at a time)
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
```
