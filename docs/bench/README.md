# Benchmark

End-to-end benchmarks for xKit, measuring real-world performance across complete scenarios.

All benchmarks run on Apple M3 Pro (12 cores, 36 GB), macOS 26.4, Clang 17, Release (`-O2`).

> For micro-benchmark results, see the **Benchmark** section at the bottom of each module's documentation page.

## Available Benchmarks

| Benchmark | Description |
| --------- | ----------- |
| [HTTP Server](http_server.md) | xKit single-threaded HTTP/1.1 server vs Go `net/http` — **152 K req/s**, +15–60% faster across all scenarios |
