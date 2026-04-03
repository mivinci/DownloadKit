<p align="center">
  <img src="assets/logo.png" alt="xKit" height="160">
</p>

<div align="center">
  <a href="diary">Diary</a>
  <span>&nbsp;&nbsp;•&nbsp;&nbsp;</span>
  <a href="TODO.md">Todo</a>
  <span>&nbsp;&nbsp;•&nbsp;&nbsp;</span>
  <a href="STYLE.md">Style</a>
  <br />
  <br />
</div>

A collection of low-level C building blocks for event-driven, asynchronous programming on **macOS** and **Linux** (Windows is on the roadmap but not a near-term priority).

- Designed and reviewed by Leo X.
- Coded by Codebuddy VSCode plugin with claude-4.6-opus

## Modules

### xbase — Core primitives

| Header | Description |
| -------- | ------------- |
| `event.h` | Cross-platform event loop (edge-triggered) — kqueue / epoll / poll |
| `timer.h` | Monotonic timer with push (thread-pool) and poll (lock-free MPSC) fire modes |
| `task.h` | N:M task model — lightweight tasks multiplexed onto a thread pool |
| `socket.h` | Async socket abstraction with idle-timeout support over xEventLoop |
| `memory.h` | Reference-counted allocation with vtable-driven lifecycle (ctor / dtor / retain / release) |
| `log.h` | Per-thread callback-based logging with optional backtrace on fatal |
| `backtrace.h` | Platform-adaptive stack trace capture (libunwind > execinfo > stub) |
| `error.h` | Unified error codes and human-readable messages |
| `heap.h` | Min-heap used internally by the timer subsystem |
| `mpsc.h` | Lock-free multi-producer / single-consumer queue |
| `atomic.h` | Compiler-portable atomic operations |

### xhttp — Async HTTP client

| Header | Description |
| -------- | ------------- |
| `client.h` | Non-blocking HTTP client powered by libcurl multi-socket + xEventLoop |
| `client_sse.c` | SSE (Server-Sent Events) streaming client with W3C-compliant event parsing |

Supports oneshot requests (GET / POST / PUT / DELETE / PATCH / HEAD) with per-request timeout, as well as SSE streaming subscriptions with event / done callbacks.

### xlog — Async logging

| Header | Description |
| -------- | ------------- |
| `logger.h` | High-performance async logger with MPSC queue, timer/pipe flush modes, and log rotation |

Features thread-local logger context (`xLoggerEnter` / `xLoggerLeave` / `xLoggerCurrent`) for convenient `XLOG_*` macro usage, three operating modes (Timer / Notify / Mixed), size-based log file rotation, and synchronous flush on fatal.

## Prerequisites

| Dependency | Required | Notes |
| ------------ | ---------- | ------- |
| CMake ≥ 3.14 | ✅ | Build system |
| C99 compiler | ✅ | GCC or Clang |
| GoogleTest | For tests | `libgtest-dev` (apt) / `googletest` (brew) |
| libcurl | Optional | Enables the **xhttp** module |
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
