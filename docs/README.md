# xKit

Welcome to the xKit documentation. xKit is a collection of low-level C building blocks for event-driven, asynchronous programming on **macOS** and **Linux**. (Windows is on the roadmap but not a near-term priority).

- Designed and reviewed by Leo X.
- Coded by Codebuddy with claude-4.6-opus

## Architecture Overview

```mermaid
graph TD
    subgraph "Application Layer"
        APP["User Application"]
    end

    subgraph "High-Level Modules"
        XHTTP["xhttp<br/>HTTP Client &amp; Server &amp; WebSocket"]
        XLOG["xlog<br/>Async Logging"]
    end

    subgraph "Networking Layer"
        XNET["xnet<br/>URL / DNS / TLS Config"]
    end

    subgraph "Buffer Layer"
        XBUF["xbuf<br/>Buffer Primitives"]
    end

    subgraph "Core Layer"
        XBASE["xbase<br/>Core Primitives"]
    end

    APP --> XHTTP
    APP --> XLOG
    APP --> XNET
    APP --> XBUF
    APP --> XBASE
    XHTTP --> XNET
    XHTTP --> XBASE
    XHTTP --> XBUF
    XNET --> XBASE
    XLOG --> XBASE
    XBUF -->|"atomic.h"| XBASE

    style XBASE fill:#50b86c,color:#fff
    style XBUF fill:#4a90d9,color:#fff
    style XNET fill:#e74c3c,color:#fff
    style XHTTP fill:#f5a623,color:#fff
    style XLOG fill:#9b59b6,color:#fff
```

## Module Index

### [xbase](xbase/index.html) — Core Primitives

The foundation of xKit. Provides event loop, timers, tasks, async sockets, memory management, and lock-free data structures.

| Sub-Module | Description |
| --- | --- |
| [event.h](xbase/event.md) | Cross-platform event loop — kqueue (macOS) / epoll (Linux) / poll (fallback) |
| [timer.h](xbase/timer.md) | Monotonic timer with Push (thread-pool) and Poll (lock-free MPSC) fire modes |
| [task.h](xbase/task.md) | N:M task model — lightweight tasks multiplexed onto a thread pool |
| [socket.h](xbase/socket.md) | Async socket abstraction with idle-timeout support |
| [memory.h](xbase/memory.md) | Reference-counted allocation with vtable-driven lifecycle |
| [error.h](xbase/error.md) | Unified error codes and human-readable messages |
| [heap.h](xbase/heap.md) | Min-heap with index tracking (used by timer subsystem) |
| [mpsc.h](xbase/mpsc.md) | Lock-free multi-producer / single-consumer queue |
| [atomic.h](xbase/atomic.md) | Compiler-portable atomic operations (GCC/Clang builtins) |
| [log.h](xbase/log.md) | Per-thread callback-based logging with optional backtrace |
| [backtrace.h](xbase/backtrace.md) | Platform-adaptive stack trace (libunwind > execinfo > stub) |
| `time.h` | Time utilities: `xMonoMs()` (monotonic) and `xWallMs()` (wall-clock) |

### [xbuf](xbuf/index.html) — Buffer Primitives

Three buffer types for different I/O patterns — linear, ring, and block-chain.

| Sub-Module | Description |
| --- | --- |
| [buf.h](xbuf/buf.md) | Linear auto-growing byte buffer with 2× expansion |
| [ring.h](xbuf/ring.md) | Fixed-size ring buffer with power-of-2 mask indexing |
| [io.h](xbuf/io.md) | Reference-counted block-chain I/O buffer with zero-copy split/cut |

### [xnet](xnet/index.html) — Networking Primitives

Shared networking utilities: URL parser, async DNS resolver, and TLS configuration types used by higher-level modules.

| Sub-Module | Description |
| --- | --- |
| [url.h](xnet/url.md) | Lightweight URL parser with zero-copy component extraction |
| [dns.h](xnet/dns.md) | Async DNS resolution via thread-pool offload |
| [tls.h](xnet/tls.md) | Shared TLS configuration types (client & server) |

### [xhttp](xhttp/index.html) — Async HTTP Client & Server & WebSocket

Full-featured async HTTP framework: libcurl-powered client with SSE streaming, event-driven server with HTTP/1.1 & HTTP/2 (h2c), TLS support (OpenSSL / mbedTLS), and RFC 6455 WebSocket (server & client).

| Sub-Module | Description |
| --- | --- |
| [client.h](xhttp/client.md) | Async HTTP client (GET / POST / PUT / DELETE / PATCH / HEAD) |
| [client_sse.c](xhttp/client_sse.md) | SSE streaming client with W3C-compliant event parsing |
| [server.h](xhttp/server.md) | Event-driven HTTP server with HTTP/1.1 and HTTP/2 (h2c) |
| [ws.h](xhttp/ws_server.md) | RFC 6455 WebSocket server with handler-initiated upgrade |
| [ws.h](xhttp/ws_client.md) | RFC 6455 WebSocket client with async connect |
| [transport.h](xhttp/tls.md) | Pluggable TLS transport layer (OpenSSL / mbedTLS / plain) |

### [xlog](xlog/index.html) — Async Logging

High-performance async logger with MPSC queue, three flush modes, and file rotation.

| Sub-Module | Description |
| --- | --- |
| [logger.h](xlog/logger.md) | Async logger with Timer / Notify / Mixed modes and `XLOG_*` macros |

## Quick Navigation Guide

### By Use Case

| I want to... | Start here |
| --- | --- |
| Build an event-driven server | [xbase/event.h](xbase/event.md) → [xbase/socket.h](xbase/socket.md) |
| Schedule timers | [xbase/timer.h](xbase/timer.md) |
| Run tasks on a thread pool | [xbase/task.h](xbase/task.md) |
| Make async HTTP requests | [xhttp/client.h](xhttp/client.md) |
| Stream LLM API responses (SSE) | [xhttp/client_sse.c](xhttp/client_sse.md) |
| Build an HTTP server | [xhttp/server.h](xhttp/server.md) |
| Add WebSocket server | [xhttp/ws.h](xhttp/ws_server.md) |
| Connect as WebSocket client | [xhttp/ws.h](xhttp/ws_client.md) |
| Parse a URL | [xnet/url.h](xnet/url.md) |
| Resolve DNS asynchronously | [xnet/dns.h](xnet/dns.md) |
| Configure TLS | [xnet/tls.h](xnet/tls.md) |
| Enable TLS (HTTPS) | [xhttp/transport.h](xhttp/tls.md) |
| Add async logging | [xlog/logger.h](xlog/logger.md) |
| Manage object lifecycles | [xbase/memory.h](xbase/memory.md) |
| Choose the right buffer type | [xbuf overview](xbuf/index.html) |
| Build a lock-free producer/consumer pipeline | [xbase/mpsc.h](xbase/mpsc.md) |

### By Dependency Level

```text
Level 0 (no deps)     : atomic.h, error.h, time.h
Level 1 (atomic only) : heap.h, mpsc.h
Level 2 (Level 0-1)   : memory.h, log.h, backtrace.h, buf.h, ring.h
Level 3 (Level 0-2)   : event.h, io.h, url.h, tls.h
Level 4 (event loop)  : timer.h, task.h, socket.h, dns.h, logger.h, client.h, server.h, ws.h
```

## Module Dependency Graph

```mermaid
graph BT
    subgraph "Level 0"
        ATOMIC["atomic.h"]
        ERROR["error.h"]
        TIME["time.h"]
    end

    subgraph "Level 1"
        HEAP["heap.h"]
        MPSC["mpsc.h"]
    end

    subgraph "Level 2"
        MEMORY["memory.h"]
        LOG["log.h"]
        BT_["backtrace.h"]
        BUF["buf.h"]
        RING["ring.h"]
    end

    subgraph "Level 3"
        EVENT["event.h"]
        IO["io.h"]
        URL["url.h"]
        TLS_CONF["tls.h"]
    end

    subgraph "Level 4"
        TIMER["timer.h"]
        TASK["task.h"]
        SOCKET["socket.h"]
        DNS["dns.h"]
        LOGGER["logger.h"]
        CLIENT["client.h"]
        SERVER["server.h"]
        WS["ws.h"]
    end

    HEAP --> ATOMIC
    MPSC --> ATOMIC
    MEMORY --> ERROR
    LOG --> BT_
    IO --> ATOMIC
    IO --> BUF
    EVENT --> HEAP
    EVENT --> MPSC
    EVENT --> TIME
    TIMER --> EVENT
    TASK --> EVENT
    SOCKET --> EVENT
    DNS --> EVENT
    LOGGER --> EVENT
    LOGGER --> MPSC
    LOGGER --> LOG
    CLIENT --> EVENT
    CLIENT --> BUF
    CLIENT --> URL
    CLIENT --> DNS
    CLIENT --> TLS_CONF
    SERVER --> SOCKET
    SERVER --> BUF
    SERVER --> TLS_CONF
    WS --> SERVER
    WS --> URL

    style EVENT fill:#50b86c,color:#fff
    style URL fill:#e74c3c,color:#fff
    style DNS fill:#e74c3c,color:#fff
    style TLS_CONF fill:#e74c3c,color:#fff
    style CLIENT fill:#f5a623,color:#fff
    style SERVER fill:#f5a623,color:#fff
    style WS fill:#f5a623,color:#fff
    style LOGGER fill:#9b59b6,color:#fff
```

## Build & Test

```bash
# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# Test
ctest --test-dir build --output-on-failure --parallel 4
```

See the project [README](../README.md) for full build instructions, prerequisites, and container-based Linux testing.

## License

[MIT](../LICENSE) © 2025-present Leo X. and xKit contributors
