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
| **[xbase](https://le0.me/xKit/modules/xbase)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](https://le0.me/xKit/modules/xbuf)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xnet](https://le0.me/xKit/modules/xnet)** | Networking primitives — URL parser, async DNS resolver, shared TLS configuration types |
| **[xhttp](https://le0.me/xKit/modules/xhttp)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, HTTP/1.1 (llhttp) & HTTP/2 (nghttp2) async server with TLS (OpenSSL / MbedTLS) and parameterized routing, WebSocket server & client (RFC 6455) |
| **[xlog](https://le0.me/xKit/modules/xlog)** | Async logging — MPSC queue, timer/pipe flush, log rotation |
| **[xcrypto](https://le0.me/xKit/modules/xcrypto)** | Cryptographic primitives — SHA-1 with pluggable backend (OpenSSL / mbedTLS / builtin) |
| **[xp2p](https://le0.me/xKit/modules/xp2p)** | P2P connectivity — ICE agent, STUN/TURN client, SDP codec, NAT traversal |
| **[xfer](https://le0.me/xKit/modules/xfer)** | P2P file transfer — zero-config send/receive over WebRTC DataChannel with signaling, chunking, SHA-1 verification, and resume support |

📖 See the **[full documentation](https://le0.me/xKit)** for detailed design, architecture diagrams, API references, and usage examples.

## Prerequisites

| Dependency | Required | Notes |
| ------------ | ---------- | ------- |
| CMake ≥ 3.14 | ✅ | Build system |
| C99 compiler | ✅ | GCC or Clang |
| OpenSSL | ✅ pick one | TLS backend for **xhttp** and **xp2p** DTLS — `libssl-dev` (apt) / `openssl` (brew) |
| MbedTLS | ✅ pick one | TLS backend for **xhttp** and **xp2p** DTLS — `libmbedtls-dev` (apt) / `mbedtls` (brew) |
| libunwind | Optional | Better backtraces on Linux — `libunwind-dev` (apt) |

The following dependencies are **automatically fetched and built from source** via CMake FetchContent if not found on the system. You can still install them manually for faster builds:

| Dependency | Description | Manual install |
| ---------- | ----------- | -------------- |
| libcurl | HTTP client for **xhttp** | `libcurl4-openssl-dev` (apt) / `curl` (brew) |
| llhttp | HTTP/1.1 parsing for **xhttp** server | `libllhttp-dev` (apt) / `llhttp` (brew) |
| nghttp2 | HTTP/2 support for **xhttp** server | `libnghttp2-dev` (apt) / `nghttp2` (brew) |
| cJSON | JSON parsing for **xfer** signaling | `libcjson-dev` (apt) / `cjson` (brew) |
| usrsctp | User-space SCTP for **xp2p** DataChannel | `libusrsctp-dev` (apt) / `usrsctp` (brew) |
| libuv | Event loop for benchmarks | `libuv1-dev` (apt) / `libuv` (brew) |
| GoogleTest | Unit testing (when `XK_BUILD_TESTS=ON`) | `libgtest-dev` (apt) / `googletest` (brew) |
| Google Benchmark | Benchmarking (when `XK_BUILD_BENCHMARKS=ON`) | `libbenchmark-dev` (apt) / `google-benchmark` (brew) |

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

Both **xhttp** and **xp2p** share the same TLS backend setting. Use `XK_TLS_BACKEND` to choose at configure time:

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

### xp2p DTLS configuration

The **xp2p** module includes DTLS + SCTP + DataChannel for WebRTC browser interop (always enabled). It reuses the same `XK_TLS_BACKEND` setting as **xhttp**:

```bash
# OpenSSL backend (default)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DXK_TLS_BACKEND=openssl

# mbedTLS backend
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DXK_TLS_BACKEND=mbedtls
```

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

All benchmarks run on Apple M3 Pro (12 cores, 36 GB), macOS 26.4, Clang 17, Release (`-O2`).

### Highlights

| Category | Highlight |
| -------- | --------- |
| MPSC Queue | **94.4 M ops/s** single-producer; 68–70 M ops/s multi-producer |
| RingBuffer | **73.3 GiB/s** write+read (4 KiB chunks) |
| IOBuffer | **44.8 GiB/s** append (4 KiB), zero-copy cut at 26.1 GiB/s |
| HTTP/1.1 Server | **152 K req/s** single-threaded, +15–60% faster than Go `net/http` |
| HTTP/2 Server | **576 K req/s** single-threaded h2c, +15–405% faster than Go `net/http` + `x/net/http2` |
| HTTPS Server | **512 K req/s** HTTPS/2, TLS-bound parity on HTTPS/1.1, +209–278% on HTTPS/2 |

### Run Benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXK_BUILD_BENCHMARKS=ON
cmake --build build --parallel
./scripts/run_micro_bench.sh
```

### Full Results

- **Micro-benchmarks** — detailed tables are in each module's documentation:
  [mpsc.h](https://le0.me/xKit/modules/xbase/mpsc.html#benchmark) · [event.h](https://le0.me/xKit/modules/xbase/event.html#benchmark) · [timer.h](https://le0.me/xKit/modules/xbase/timer.html#benchmark) · [heap.h](https://le0.me/xKit/modules/xbase/heap.html#benchmark) · [memory.h](https://le0.me/xKit/modules/xbase/memory.html#benchmark) · [buf.h](https://le0.me/xKit/modules/xbuf/buf.html#benchmark) · [ring.h](https://le0.me/xKit/modules/xbuf/ring.html#benchmark) · [io.h](https://le0.me/xKit/modules/xbuf/io.html#benchmark)
- **End-to-end** — [HTTP/1.1 Server Benchmark](https://le0.me/xKit/bench/http_server.html) · [HTTP/2 Server Benchmark](https://le0.me/xKit/bench/http2_server.html) · [HTTPS Server Benchmark](https://le0.me/xKit/bench/https_server.html) (xKit vs Go)

## License

[MIT](LICENSE) © 2025-present Leo X. and xKit contributors
