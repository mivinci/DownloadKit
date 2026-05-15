/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http_bench_server.cpp - HTTP server for end-to-end benchmarking
 *
 * Usage: http_bench_server [port]
 *   Default port: 8080
 *
 * Routes:
 *   GET  /ping          → 200 "pong"  (minimal response)
 *   GET  /echo?size=N   → 200 with N bytes of 'x' (payload test)
 *   POST /echo          → 200 echoing the request body
 *
 * Designed to be benchmarked with wrk, ab, or hey:
 *   wrk -t4 -c100 -d10s http://127.0.0.1:8080/ping
 *   hey -n 100000 -c 50 http://127.0.0.1:8080/ping
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <vector>

extern "C" {
#include <xbase/event.h>
#include <xhttp/server.h>
}

static xEventLoop g_loop = nullptr;

// GET /ping → "pong"
static void handle_ping(xHttpResponseWriter writer, const xHttpRequest *req,
                        void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseSetHeader(writer, "Content-Type", "text/plain");
  xHttpResponseSend(writer, "pong", 4);
}

// GET /echo?size=N → N bytes of 'x'
// POST /echo → echo request body
static void handle_echo(xHttpResponseWriter writer, const xHttpRequest *req,
                        void *arg) {
  (void)arg;

  if (strcmp(req->method, "POST") == 0) {
    // Echo back the request body
    xHttpResponseSetHeader(writer, "Content-Type", "application/octet-stream");
    xHttpResponseSend(writer, req->body, req->body_len);
    return;
  }

  // GET: parse ?size=N from URL
  size_t size = 64; // default
  const char *q = strchr(req->url, '?');
  if (q) {
    const char *sp = strstr(q, "size=");
    if (sp) {
      size = (size_t)atoi(sp + 5);
      if (size == 0) size = 64;
      if (size > 1048576) size = 1048576; // cap at 1MB
    }
  }

  std::vector<char> body(size, 'x');
  xHttpResponseSetHeader(writer, "Content-Type", "application/octet-stream");
  xHttpResponseSend(writer, body.data(), body.size());
}

int main(int argc, char *argv[]) {
  uint16_t port = 8080;
  if (argc > 1) port = (uint16_t)atoi(argv[1]);

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  // Watch SIGINT to stop gracefully
  xEventLoopSignalWatch(g_loop, SIGINT,
                        [](int, void *) { xEventLoopStop(g_loop); }, nullptr);

  xHttpServer server = xHttpServerCreate(g_loop);
  if (!server) {
    fprintf(stderr, "Failed to create HTTP server\n");
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xHttpServerRoute(server, "GET /ping", handle_ping, nullptr);
  xHttpServerRoute(server, "/echo", handle_echo, nullptr);

  xErrno err = xHttpServerListen(server, "0.0.0.0", port);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to listen on port %u\n", port);
    xHttpServerDestroy(server);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  fprintf(stdout, "HTTP bench server listening on 0.0.0.0:%u\n", port);
  fprintf(stdout, "Routes:\n");
  fprintf(stdout, "  GET  /ping        → 200 \"pong\"\n");
  fprintf(stdout, "  GET  /echo?size=N → 200 with N bytes\n");
  fprintf(stdout, "  POST /echo        → 200 echo body\n");
  fflush(stdout);

  xEventLoopRun(g_loop);

  xHttpServerDestroy(server);
  xEventLoopDestroy(g_loop);
  return 0;
}
