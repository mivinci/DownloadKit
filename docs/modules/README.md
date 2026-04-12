# Modules

xKit is organized into seven modules, layered from low-level core primitives up to high-level async networking and P2P connectivity.

```text
┌─────────────────────────────────────────────┐
│              Application Layer              │
├──────────────────────┬──────────────────────┤
│   xhttp              │   xlog               │
│   HTTP Client/Server │   Async Logging       │
│   WebSocket          │                      │
├──────────────────────┼──────────────────────┤
│   xp2p               │                      │
│   ICE / STUN / TURN  │                      │
├──────────────────────┴──────────────────────┤
│   xnet — URL / DNS / TCP / TLS Config       │
├─────────────────────────────────────────────┤
│   xbuf — Linear / Ring / Block-Chain Buffer │
├──────────────────────┬──────────────────────┤
│   xbase              │   xcrypto            │
│   Event Loop / Timer │   SHA-1 / Crypto     │
│   Task / Memory      │   Primitives         │
└──────────────────────┴──────────────────────┘
```

## Overview

| Module | Description |
| ------ | ----------- |
| **[xbase](xbase/README.md)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](xbuf/README.md)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xnet](xnet/README.md)** | Networking primitives — URL parser, async DNS resolver, TCP, shared TLS configuration types |
| **[xhttp](xhttp/README.md)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, HTTP/1.1 & HTTP/2 async server with TLS, WebSocket server & client |
| **[xlog](xlog/README.md)** | Async logging — MPSC queue, timer/pipe flush, log rotation |
| **[xcrypto](xcrypto/README.md)** | Cryptographic primitives — SHA-1 with pluggable backend (OpenSSL / mbedTLS / builtin) |
| **[xp2p](xp2p/README.md)** | P2P connectivity — ICE agent, STUN/TURN client, SDP codec, NAT traversal |

## Dependency Order

```text
Level 0 (no deps)     : atomic.h, error.h, time.h
Level 1 (atomic only) : heap.h, mpsc.h
Level 2 (Level 0-1)   : memory.h, log.h, backtrace.h, buf.h, ring.h
Level 3 (Level 0-2)   : event.h, io.h, url.h, tls.h
Level 4 (event loop)  : timer.h, task.h, socket.h, dns.h, tcp.h, logger.h, client.h, server.h, ws.h
Level 5 (xbase+xnet) : ice_agent.h, stun_msg.h, stun_attr.h, stun_txn.h, turn_client.h, sdp.h
Level ∞ (standalone)  : sha1.h (xcrypto — depends only on xbase error codes)
```
