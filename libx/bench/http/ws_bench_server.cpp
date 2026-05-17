/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_bench_server.cpp - WebSocket echo server for end-to-end benchmarking
 *
 * Usage: ws_bench_server [port]
 *   Default port: 9090
 *
 * Behavior:
 *   Accepts WebSocket connections on any path.
 *   Echoes every received message (text or binary) back to the sender.
 *
 * Designed to be benchmarked with:
 *   - Custom Go benchmark client (ws_bench_client.go)
 *   - tcpkali with WebSocket mode
 *   - wscat for manual testing
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <x/base/event.h>
#include <x/http/server.h>
#include <x/http/ws.h>
}

static xEventLoop            g_loop = nullptr;
static std::atomic<uint64_t> g_connections{0};
static std::atomic<uint64_t> g_messages{0};

/* ── WebSocket callbacks ───────────────────────────────── */

static void on_open(xWsConn conn, void *arg) {
  (void)conn;
  (void)arg;
  g_connections.fetch_add(1, std::memory_order_relaxed);
}

static void on_message(xWsConn conn, xWsOpcode opcode, const void *payload, size_t len, void *arg) {
  (void)arg;
  g_messages.fetch_add(1, std::memory_order_relaxed);
  /* Echo the message back */
  xWsSend(conn, opcode, payload, len);
}

static void on_close(xWsConn conn, uint16_t code, const char *reason, size_t len, void *arg) {
  (void)conn;
  (void)code;
  (void)reason;
  (void)len;
  (void)arg;
}

static const xWsCallbacks ws_cbs = {
  .on_open    = on_open,
  .on_message = on_message,
  .on_close   = on_close,
};

int main(int argc, char *argv[]) {
  uint16_t port = 9090;
  if (argc > 1) port = (uint16_t)atoi(argv[1]);

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  /* Watch SIGINT to stop gracefully */
  xEventLoopSignalWatch(g_loop, SIGINT, [](int, void *) { xEventLoopStop(g_loop); }, nullptr);

  xHttpServer server = xWsServe(g_loop, "0.0.0.0", port, &ws_cbs, nullptr);
  if (!server) {
    fprintf(stderr, "Failed to create WebSocket server on port %u\n", port);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  fprintf(stdout, "WebSocket bench server listening on ws://0.0.0.0:%u/\n", port);
  fprintf(stdout, "Behavior: echo all messages back to sender\n");
  fflush(stdout);

  xEventLoopRun(g_loop);

  fprintf(stdout, "\nTotal connections: %llu\n", (unsigned long long)g_connections.load());
  fprintf(stdout, "Total messages:    %llu\n", (unsigned long long)g_messages.load());

  xHttpServerDestroy(server);
  xEventLoopDestroy(g_loop);
  return 0;
}
