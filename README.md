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

## License

[MIT](LICENSE) © 2025-present Leo X. and xKit contributors
