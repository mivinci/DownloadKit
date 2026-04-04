<!-- markdownlint-disable MD033 -->
<!-- markdownlint-disable MD041 -->
<p align="center">
  <img src="docs/logo.png" alt="xKit" height="160">
</p>

<div align="center">
  <a href="docs/README.md">Docs</a>
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
| **[xbase](docs/xbase/README.md)** | Core primitives — event loop, timers, tasks, async sockets, memory, lock-free data structures |
| **[xbuf](docs/xbuf/README.md)** | Buffer primitives — linear, ring, and block-chain I/O buffers |
| **[xhttp](docs/xhttp/README.md)** | Async HTTP client & server — libcurl multi-socket client with SSE streaming, llhttp-based async server with parameterized routing |
| **[xlog](docs/xlog/README.md)** | Async logging — MPSC queue, timer/pipe flush, log rotation |

📖 See the **[full documentation](docs/README.md)** for detailed design, architecture diagrams, API references, and usage examples.

## Prerequisites

| Dependency | Required | Notes |
| ------------ | ---------- | ------- |
| CMake ≥ 3.14 | ✅ | Build system |
| C99 compiler | ✅ | GCC or Clang |
| GoogleTest | For tests | `libgtest-dev` (apt) / `googletest` (brew) |
| libcurl | Optional | Enables the **xhttp** client |
| llhttp | Optional | Enables the **xhttp** server (HTTP parsing) |
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
