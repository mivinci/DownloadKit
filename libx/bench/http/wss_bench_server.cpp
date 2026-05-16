/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * wss_bench_server.cpp - WebSocket-over-TLS echo server for benchmarking
 *
 * Usage: wss_bench_server [port] [cert] [key]
 *   Default port: 9090
 *   Default cert: bench_cert.pem (in current directory)
 *   Default key:  bench_key.pem  (in current directory)
 *
 * Behavior:
 *   Accepts WSS connections on any path.
 *   Echoes every received message (text or binary) back to the sender.
 *
 * Designed to be benchmarked with:
 *   - Custom Go benchmark client (ws_bench_client.go) using wss:// URL
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <x/base/event.h>
#include <x/http/server.h>
#include <x/http/ws.h>
#include <x/net/tls.h>
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

/* ── HTTP handler that upgrades to WebSocket ───────────── */

static void ws_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  xWsUpgrade(w, req, &ws_cbs, nullptr);
}

int main(int argc, char *argv[]) {
  uint16_t    port      = 9090;
  const char *cert_path = "bench_cert.pem";
  const char *key_path  = "bench_key.pem";

  if (argc > 1) port = (uint16_t)atoi(argv[1]);
  if (argc > 2) cert_path = argv[2];
  if (argc > 3) key_path = argv[3];

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  /* Watch SIGINT to stop gracefully */
  xEventLoopSignalWatch(g_loop, SIGINT, [](int, void *) { xEventLoopStop(g_loop); }, nullptr);

  xHttpServer server = xHttpServerCreate(g_loop);
  if (!server) {
    fprintf(stderr, "Failed to create HTTP server\n");
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xHttpServerRoute(server, "GET /", ws_handler, nullptr);

  xTlsConf tls = {};
  tls.cert     = cert_path;
  tls.key      = key_path;

  /* WebSocket upgrade requires HTTP/1.1 — disable h2 ALPN */
  static const char *ws_alpn[] = {"http/1.1", NULL};
  tls.alpn                     = ws_alpn;
  tls.skip_verify              = 1; /* No client cert required for benchmarking */

  xErrno err = xHttpServerListenTls(server, "0.0.0.0", port, &tls);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to listen TLS on port %u (err=%d)\n", port, err);
    fprintf(stderr, "Make sure cert and key files exist:\n");
    fprintf(stderr, "  cert: %s\n", cert_path);
    fprintf(stderr, "  key:  %s\n", key_path);
    fprintf(stderr, "Generate with:\n");
    fprintf(stderr,
            "  openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
            "-days 365 -nodes -subj '/CN=localhost'\n",
            key_path, cert_path);
    xHttpServerDestroy(server);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  fprintf(stdout, "WSS bench server listening on wss://0.0.0.0:%u/\n", port);
  fprintf(stdout, "Behavior: echo all messages back to sender\n");
  fprintf(stdout, "TLS cert: %s\n", cert_path);
  fprintf(stdout, "TLS key:  %s\n", key_path);
  fflush(stdout);

  xEventLoopRun(g_loop);

  fprintf(stdout, "\nTotal connections: %llu\n", (unsigned long long)g_connections.load());
  fprintf(stdout, "Total messages:    %llu\n", (unsigned long long)g_messages.load());

  xHttpServerDestroy(server);
  xEventLoopDestroy(g_loop);
  return 0;
}
