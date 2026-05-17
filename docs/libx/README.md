# Libraries

moo is organized into nine libraries, layered from low-level core primitives up to high-level async networking, P2P connectivity, file transfer, and an embeddable JavaScript engine.

```text
┌─────────────────────────────────────────────┐
│              Application Layer              │
├──────────────────────┬──────────────────────┤
│   xfer               │   xjs                │
│   P2P File Transfer  │   JS Scripting (QJS) │
├──────────────────────┼──────────────────────┤
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
│   Event Loop / Timer │   SHA-1/256 MD5 CRC  │
│   Task / Memory      │   HMAC / Crypto      │
└──────────────────────┴──────────────────────┘
```

## Overview

| Library | Description |
| ------- | ----------- |
| **[xbase](base/README.md)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](buf/README.md)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xnet](net/README.md)** | Networking primitives — URL parser, async DNS resolver, TCP, shared TLS configuration types |
| **[xhttp](http/README.md)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, HTTP/1.1 & HTTP/2 async server with TLS, WebSocket server & client |
| **[xlog](log/README.md)** | Async logging — MPSC queue, timer/pipe flush, log rotation |
| **[xjs](js/README.md)** | Embeddable JavaScript engine — QuickJS-ng backend, JSC-shaped C API, ES modules, native class wrappers |
| **[xcrypto](crypto/README.md)** | Cryptographic primitives — SHA-1, SHA-256 (OpenSSL / mbedTLS / builtin), MD5, CRC-32, generic HMAC with HMAC-SHA1, HMAC-SHA256, HMAC-MD5 |
| **[xp2p](p2p/README.md)** | P2P connectivity — ICE agent, STUN/TURN client, SDP codec, NAT traversal |
| **[xfer](fer/README.md)** | P2P file transfer — chunked transfer over WebRTC DataChannel with signaling, resume, and SHA-1 integrity |

## Dependency Order

```text
Level 0 (no deps)     : atomic.h, error.h, time.h
Level 1 (atomic only) : heap.h, mpsc.h
Level 2 (Level 0-1)   : memory.h, log.h, backtrace.h, buf.h, ring.h
Level 3 (Level 0-2)   : event.h, io.h, url.h, tls.h
Level 4 (event loop)  : timer.h, task.h, socket.h, dns.h, tcp.h, logger.h, client.h, server.h, ws.h
Level 5 (xbase+xnet) : ice_agent.h, stun_msg.h, stun_attr.h, stun_txn.h, turn_client.h, sdp.h
Level 6 (xp2p+xhttp) : xfer.h, xfer_signal.h, xfer_protocol.h
Level ∞ (standalone)  : sha1.h, sha256.h, md5.h, crc32.h, hmac.h (xcrypto — depends only on xbase error codes)
Level ∞ (standalone)  : js.h                                      (xjs     — depends only on xbase; pulls QuickJS-ng privately)
```
