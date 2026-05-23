# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is moo

moo is a self-contained AI agent runtime written in C with a streaming REPL CLI. It connects to any OpenAI-compatible endpoint (Kimi, GLM, DeepSeek, etc.) and provides tool calls, token budgeting, sidecar queries, and layered memory. The foundation libraries (`libx/`) are independently reusable C modules.

## Build Commands

```bash
# Full debug build (libraries + tests + benchmarks)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel

# App-only release build (the `moo` CLI)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DMOO_BUILD_APPS=ON -DX_BUILD_TESTS=OFF -DX_BUILD_BENCHMARKS=OFF
cmake --build build --parallel

# Shortcut via package.json (builds apps + examples, debug level 1)
npm run build
```

Key CMake options: `MOO_BUILD_APPS` (OFF), `X_BUILD_TESTS` (ON), `X_BUILD_BENCHMARKS` (ON), `X_BUILD_EXAMPLES` (OFF), `X_BUILD_STATIC` (OFF), `X_TLS_BACKEND` (auto/openssl/mbedtls/none), `X_DEBUG_LEVEL` (0-3), `MOO_ENABLE_ASAN` (OFF).

## Test Commands

```bash
# Run all tests
ctest --test-dir build --output-on-failure --parallel 4

# Run a single test binary directly
./build/libx/x/base/xbase_test --gtest_filter="HeapTest.*"

# Affected-modules only (faster iteration vs origin/main)
./scripts/test-mac.sh
./scripts/test-mac.sh -t mbedtls --all

# Test both TLS backends
cmake -S . -B build-openssl -DX_TLS_BACKEND=openssl && cmake --build build-openssl --parallel && ctest --test-dir build-openssl --output-on-failure --parallel 4
cmake -S . -B build-mbedtls -DX_TLS_BACKEND=mbedtls && cmake --build build-mbedtls --parallel && ctest --test-dir build-mbedtls --output-on-failure --parallel 4

# Linux testing via Apple Containerization (macOS 26+)
./scripts/test-linux.sh
```

## Run the CLI

```bash
./build/cli/moo --data-dir ~/.moo
```

First run scaffolds `models.json` — fill in API keys, then re-run.

## Architecture

```
cli/            → The `moo` REPL app (C++, built with MOO_BUILD_APPS=ON)
libx/x/agent/  → Agent core: agent, session, query, tool, budget, provider, model, memory
libx/x/base/   → Event loop (kqueue/epoll/poll), timers, tasks, atomics, data structures
libx/x/buf/    → Linear, ring, chain I/O buffers
libx/x/net/    → URL parser, async DNS, TCP, TLS transport
libx/x/http/   → libcurl multi-socket client with SSE; HTTP/1.1+HTTP/2 server; WebSocket
libx/x/line/   → CJK-aware line editor with history and reverse search
libx/x/log/    → Async MPSC logger with rotation
libx/x/tui/    → Streaming markdown → ANSI transformer
libx/x/crypto/ → SHA-1/SHA-256/MD5/CRC-32/HMAC
libx/x/js/     → Embedded JavaScript (QuickJS-ng)
libx/x/p2p/    → ICE/STUN/TURN/SDP/DTLS/SCTP/DataChannel
libx/x/fer/    → P2P file transfer over WebRTC DataChannel
libx++/xpp/    → Optional C++14 RAII layer (Own, NonNull, Option, Result)
```

### Agent subsystem (`libx/x/agent/`)

- **agent.{h,c}** — Long-lived persona (provider, model, system prompt, tool set, limits). Mints sessions.
- **session.{h,c}** — Stateful conversation. Owns history, runs the tool-call loop, emits callbacks (on_text/on_thinking/on_tool/on_done).
- **query.{h,c}** — Single round-trip to model with streaming decode and sidecar supervision.
- **provider.{h,c} / provider_openai.c** — Backend vtable + OpenAI-compatible implementation.
- **model.{h,c}** — Model registry enabling runtime model switching via `/model <id>`.
- **tool.{h,c} / tool_shell.{h,c}** — Tool definition ABI + built-in shell tool with confirmation hooks.
- **budget.{h,c}** — Prompt-size estimator, rolling trimmer, auto-calibrator.
- **memory.{h,c} / memory_jsonl.c** — Pluggable session memory with JSONL backend.

### CLI structure (`cli/`)

- **main.cpp** — Entry: argv parsing, object assembly, event loop
- **repl.{h,cpp}** — REPL loop using xline
- **callbacks.{h,cpp}** — Agent session event handlers
- **slash.{h,cpp}** — Slash command dispatch
- **slash_*.cpp** — Individual command implementations (model, bypass, renderer, verbose)
- **config.{h,cpp}** — models.json loading
- **output.{h,cpp}** — Rendered output via xtui

### Dependency graph (libraries)

`xagent` → `xhttp` → `xnet` → `xbase`; `xhttp` also uses `xbuf`. Each module links as shared by default; `xagent` privately links cJSON.

## Coding Conventions

- **Language**: C99 for libraries, C++ (Google Test) for tests, C++ for cli/
- **Formatting**: `.clang-format` (LLVM-based, 100-col, 2-space indent, pointer-right `int *p`)
- **Naming**: Public API uses `x` prefix + PascalCase (`xHeapPush`, `xEventLoopCreate`). Internal/static functions use snake_case. Callbacks end with `Func`, configs with `Conf`.
- **File naming**: `<module>.h` (public), `<module>_private.h` (internal), `<module>.c`, `<module>_test.cpp`
- **Header guards**: `#ifndef XBASE_EVENT_H` / `#define XBASE_EVENT_H`
- **Include order**: corresponding public header → stdlib → system → project-internal private headers
- **Public headers**: use `<x/base/xxx.h>` angle-bracket paths; private headers use `"xxx.h"`
- **Error handling**: Return `xErrno` enum. Create functions return pointer/handle (NULL on failure). Multi-step init uses `goto fail` pattern.
- **Export macro**: `XCAPI(T)` on all public functions
- **Type macros**: `XDEF_STRUCT(T)`, `XDEF_ENUM(T)`, `XDEF_HANDLE(T)`

## Commit Convention

Conventional Commits format: `<type>(<scope>): <subject>`

Types: feat, fix, docs, chore, ci, refactor, test, style

Scopes: xbase, xbuf, xnet, xhttp, xlog, xcrypto, xp2p, xfer, xagent, cli (or omit for cross-module)

## Branch Naming

`<author>/<short-description>` — all lowercase with hyphens (e.g., `mivinci/add-sse-support`).

## Compiler Warnings

C code compiles with `-Wall -Wextra -Werror`. C++ test/benchmark code uses `-Wall -Wextra` without `-Werror`.
