# Modules

xKit is organized into five modules, layered from low-level core primitives up to high-level async networking.

```text
┌─────────────────────────────────────────────┐
│              Application Layer              │
├──────────────────────┬──────────────────────┤
│   xhttp              │   xlog               │
│   HTTP Client/Server │   Async Logging       │
│   WebSocket          │                      │
├──────────────────────┴──────────────────────┤
│   xnet — URL / DNS / TCP / TLS Config       │
├─────────────────────────────────────────────┤
│   xbuf — Linear / Ring / Block-Chain Buffer │
├─────────────────────────────────────────────┤
│   xbase — Event Loop / Timer / Task /       │
│           Memory / Atomic / MPSC Queue      │
└─────────────────────────────────────────────┘
```

## Overview

| Module | Description |
| ------ | ----------- |
| **[xbase](xbase/README.md)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](xbuf/README.md)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xnet](xnet/README.md)** | Networking primitives — URL parser, async DNS resolver, TCP, shared TLS configuration types |
| **[xhttp](xhttp/README.md)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, HTTP/1.1 & HTTP/2 async server with TLS, WebSocket server & client |
| **[xlog](xlog/README.md)** | Async logging — MPSC queue, timer/pipe flush, log rotation |

## Dependency Order

```text
Level 0 (no deps)     : atomic.h, error.h, time.h
Level 1 (atomic only) : heap.h, mpsc.h
Level 2 (Level 0-1)   : memory.h, log.h, backtrace.h, buf.h, ring.h
Level 3 (Level 0-2)   : event.h, io.h, url.h, tls.h
Level 4 (event loop)  : timer.h, task.h, socket.h, dns.h, tcp.h, logger.h, client.h, server.h, ws.h
```
