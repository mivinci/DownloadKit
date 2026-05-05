# HTTPS Server Benchmark

End-to-end HTTPS server benchmark comparing **moo** (single-threaded event-loop, OpenSSL) against **Go** `net/http` + `crypto/tls` (goroutine-per-connection). Tests both HTTPS/1.1 (wrk) and HTTPS/2 (h2load with ALPN).

## Test Environment

| Item | Value |
| --- | --- |
| CPU | Apple M3 Pro (12 cores) |
| Memory | 36 GB |
| OS | macOS 26.4 (Darwin) |
| Compiler | Apple Clang 17.0.0 |
| Build | Release (`-O2`) |
| TLS Backend | OpenSSL 3.6.1 (moo), Go crypto/tls (Go) |
| Certificate | RSA 2048-bit self-signed, TLS 1.3 |
| Load Generator | [wrk](https://github.com/wg/wrk) (HTTP/1.1 over TLS), [h2load](https://nghttp2.org/documentation/h2load-howto.html) (HTTP/2 over TLS with ALPN) |

## Server Implementations

### moo (`bench/https_bench_server.cpp`)

Single-threaded event-loop HTTPS server built on `xbase/event.h` + `xhttp/server.h` + OpenSSL. Uses `xHttpServerListenTls()` which automatically sets ALPN to `{"h2", "http/1.1"}`, so the same server handles both HTTPS/1.1 and HTTPS/2 depending on client negotiation.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOO_BUILD_BENCHMARKS=ON
cmake --build build --parallel
openssl req -x509 -newkey rsa:2048 -keyout bench_key.pem -out bench_cert.pem \
  -days 365 -nodes -subj '/CN=localhost'
./build/bench/https_bench_server 8443 bench_cert.pem bench_key.pem
```

### Go (`bench/https_bench_server.go`)

Standard `net/http` server with `crypto/tls` and `x/net/http2.ConfigureServer()`. Go's TLS implementation is in pure Go (`crypto/tls`), while moo uses OpenSSL's C implementation. Both servers configure ALPN for `h2` and `http/1.1`.

```bash
cd bench && go build -o ../build/bench/go_https_bench https_bench_server.go
./build/bench/go_https_bench 8444 bench_cert.pem bench_key.pem
```

### Routes

Both servers implement identical routes:

| Route | Method | Description |
| --- | --- | --- |
| `/ping` | GET | Returns `"pong"` (4 bytes) — minimal response latency test |
| `/echo?size=N` | GET | Returns N bytes of `'x'` — variable response size test |
| `/echo` | POST | Echoes request body — request body throughput test |

## Results

### HTTPS/1.1 — GET /ping (wrk, varying connections)

Tests HTTPS/1.1 performance where each connection maintains its own TLS session. wrk reuses connections (no per-request handshake), so this measures encrypted request/response throughput.

| Connections | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 50 | 125,147 | 125,076 | 395 μs | 372 μs | ≈ 0% |
| 100 | 124,593 | 128,277 | 0.86 ms | 764 μs | Go +3% |
| 200 | 122,837 | 127,075 | 1.88 ms | 1.57 ms | Go +3% |
| 500 | 111,397 | 122,498 | 5.25 ms | 4.06 ms | Go +10% |

**Analysis:**

- Under HTTPS/1.1, **moo and Go are nearly identical** at low connection counts (~125K req/s each). This is a dramatic contrast to plaintext HTTP/1.1 where moo was +18–24% faster.
- **TLS encryption is the bottleneck**, not the HTTP layer. OpenSSL's AES-GCM encryption on a single thread saturates at ~125K req/s regardless of the HTTP framework above it.
- At 500 connections, **Go pulls ahead by ~10%** because Go's multi-threaded runtime can parallelize TLS encryption across all CPU cores, while moo's single-threaded event loop is limited to one core for both TLS and HTTP processing.
- moo's latency is slightly higher at high connection counts (5.25 ms vs 4.06 ms at 500 connections) — the single thread must serialize all TLS encrypt/decrypt operations.

### HTTPS/2 — GET /ping (h2load, varying connections)

Tests HTTPS/2 performance with TLS + ALPN negotiation. HTTP/2 multiplexing reduces the number of TLS sessions needed, which should benefit the single-threaded moo.

| Connections | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 50 | **511,586** | 165,341 | 975 μs | 2.99 ms | moo +209% |
| 100 | **508,685** | 144,024 | 1.96 ms | 6.88 ms | moo +253% |
| 200 | **497,775** | 131,749 | 4.01 ms | 15.00 ms | moo +278% |

**Analysis:**

- With HTTPS/2, **moo regains its massive advantage**: +209% to +278% over Go. HTTP/2 multiplexing means fewer TLS sessions are needed — multiple streams share one encrypted connection, so the TLS overhead is amortized.
- moo achieves **~510K req/s** over HTTPS/2 — only ~10% less than its h2c (cleartext HTTP/2) performance of 562K. The TLS overhead is minimal when amortized across multiplexed streams.
- Go's HTTPS/2 throughput (~131–165K) is comparable to its h2c numbers (~121–142K), suggesting Go's TLS overhead is also well-amortized but the HTTP/2 processing itself is the bottleneck.

### HTTPS/2 — GET /echo (h2load, varying response size)

Tests response serialization + TLS encryption throughput with different payload sizes. Fixed at 100 connections.

| Response Size | moo Req/s | Go Req/s | moo Latency | Go Latency | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 64 B | **470,607** | 146,727 | 2.11 ms | 6.74 ms | moo +221% |
| 1 KiB | **388,828** | 140,926 | 2.56 ms | 6.99 ms | moo +176% |
| 4 KiB | **227,414** | 118,595 | 4.38 ms | 8.22 ms | moo +92% |

**Analysis:**

- moo's advantage narrows as response size grows (from +221% at 64B to +92% at 4KB) because TLS encryption of larger payloads becomes a bigger fraction of total work.
- At 4KB responses, moo still achieves **893 MB/s** encrypted throughput vs Go's 466 MB/s.

### HTTPS/2 — POST /echo (h2load, varying body size)

Tests request body parsing + TLS decryption/encryption throughput. Fixed at 100 connections.

| Body Size | moo Req/s | Go Req/s | moo Transfer/s | Go Transfer/s | Δ |
| ---: | ---: | ---: | ---: | ---: | --- |
| 1 KiB | **291,086** | 146,916 | 289.93 MB/s | 147.01 MB/s | moo +98% |
| 4 KiB | **128,229** | 104,892 | 503.54 MB/s | 413.20 MB/s | moo +22% |
| 16 KiB | 38,975 | 37,391 | 609.97 MB/s | 586.70 MB/s | moo +4% |
| 64 KiB | 10,278 | **14,994** | 643.30 MB/s | **939.77 MB/s** | Go +46% |

**Analysis:**

- At small payloads (1KB), moo is +98% faster. At medium payloads (4KB), the gap narrows to +22%.
- At 16KB, the two are nearly tied (+4%). At **64KB, Go wins by +46%** — this is the first scenario where Go decisively beats moo.
- The 64KB crossover happens because: (1) TLS encryption of 64KB payloads is CPU-intensive and benefits from Go's multi-core parallelism, (2) HTTP/2 flow control window (default 64KB) creates back-pressure that the single-threaded event loop handles less efficiently than Go's goroutine scheduler.

## Protocol Comparison

How does TLS affect performance for each protocol? (GET /ping, 100 connections)

| Server | HTTP/1.1 | HTTPS/1.1 | Δ (TLS cost) |
| --- | ---: | ---: | --- |
| moo | 152,316 | 124,593 | −18% |
| Go | 128,915 | 128,277 | −0.5% |

| Server | h2c | HTTPS/2 | Δ (TLS cost) |
| --- | ---: | ---: | --- |
| moo | 561,825 | 508,685 | −9% |
| Go | 120,732 | 144,024 | +19% |

**Key Insights:**

1. **TLS costs moo 18% on HTTP/1.1** because every connection requires its own TLS session, and all encryption runs on a single thread. Go's multi-core TLS is essentially free (−0.5%).
2. **TLS costs moo only 9% on HTTP/2** because multiplexed streams share TLS sessions. This is why HTTPS/2 is moo's sweet spot.
3. **Go actually gets faster with HTTPS/2 vs h2c** (+19%) — likely because TLS session caching and ALPN negotiation provide a more optimized code path in Go's `crypto/tls` + `x/net/http2` stack.

## Summary

```text
                    moo vs Go HTTPS (Release build, OpenSSL 3.6.1)
                    =================================================

  HTTPS/1.1 (wrk):
    GET /ping:     Go ≈ moo (−0% to +10% Go advantage at high connections)
    GET /echo 1KB: Go +10%

  HTTPS/2 (h2load -m10):
    GET /ping:     moo +209% ~ +278%
    GET /echo:     moo +92%  ~ +221%
    POST /echo:    moo +98%  (1KB) → Go +46% (64KB)

  Peak throughput:  moo 512K req/s  (HTTPS/2 GET /ping, 50 connections)
  Peak transfer:    Go 940 MB/s      (HTTPS/2 POST /echo, 64KB body)
```

**Key Takeaways:**

1. **HTTPS/1.1 is TLS-bound.** Single-threaded OpenSSL encryption caps moo at ~125K req/s — the same as Go. The HTTP framework advantage disappears when TLS dominates.
2. **HTTPS/2 restores moo's advantage.** Stream multiplexing amortizes TLS overhead across streams, letting moo's efficient event loop shine again (+209–278% on GET /ping).
3. **Large payloads favor Go.** At 64KB POST bodies, Go's multi-core TLS parallelism wins by +46%. This is the only scenario where Go decisively beats moo.
4. **Choose your protocol wisely.** For latency-sensitive APIs with small payloads, HTTPS/2 + moo is optimal. For bulk data transfer, Go's multi-core TLS is more efficient.

## Reproducing

```bash
# Build moo server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOO_BUILD_BENCHMARKS=ON
cmake --build build --parallel

# Build Go HTTPS server
cd bench && go build -o ../build/bench/go_https_bench https_bench_server.go && cd ..

# Generate self-signed certificate
openssl req -x509 -newkey rsa:2048 -keyout /tmp/bench_key.pem \
  -out /tmp/bench_cert.pem -days 365 -nodes -subj '/CN=localhost'

# Install tools (macOS)
brew install wrk nghttp2

# Start servers
./build/bench/https_bench_server 8443 /tmp/bench_cert.pem /tmp/bench_key.pem &
./build/bench/go_https_bench 8444 /tmp/bench_cert.pem /tmp/bench_key.pem &

# HTTPS/1.1 benchmark (wrk)
wrk -t4 -c100 -d10s https://127.0.0.1:8443/ping
wrk -t4 -c100 -d10s https://127.0.0.1:8444/ping

# HTTPS/2 benchmark (h2load)
h2load -t4 -c100 -m10 -D 10 https://127.0.0.1:8443/ping
h2load -t4 -c100 -m10 -D 10 https://127.0.0.1:8444/ping

# POST benchmark
dd if=/dev/zero bs=4096 count=1 | tr '\0' 'x' > /tmp/body_4k.bin
h2load -t4 -c100 -m10 -D 10 -d /tmp/body_4k.bin https://127.0.0.1:8443/echo
h2load -t4 -c100 -m10 -D 10 -d /tmp/body_4k.bin https://127.0.0.1:8444/echo

# Cleanup
pkill -f https_bench_server
pkill -f go_https_bench
```
