# Benchmark

End-to-end benchmarks for moo, measuring real-world performance across complete scenarios.

All benchmarks run on Apple M3 Pro (12 cores, 36 GB), macOS 26.4, Clang 17, Release (`-O2`).

> For micro-benchmark results, see the **Benchmark** section at the bottom of each module's documentation page.

## Available Benchmarks

| Benchmark | Description |
| --------- | ----------- |
| [HTTP Server](http_server.md) | moo single-threaded HTTP/1.1 server vs Go `net/http` — **152 K req/s**, +15–60% faster across all scenarios |
| [HTTP/2 Server](http2_server.md) | moo single-threaded h2c server vs Go `net/http` + `x/net/http2` — **576 K req/s**, +15–405% faster across all scenarios |
| [HTTPS Server](https_server.md) | moo single-threaded HTTPS server vs Go `net/http` + `crypto/tls` — **512 K req/s** (HTTPS/2), TLS-bound parity on HTTPS/1.1 |
| [WebSocket Server](ws_server.md) | moo single-threaded WS echo server vs Go gorilla/websocket, nhooyr/websocket, gobwas/ws — **220 K msg/s**, +18–27% faster than best Go library |
