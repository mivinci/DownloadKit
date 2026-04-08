/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * https_bench_server.cpp - HTTPS server for end-to-end benchmarking
 *
 * Usage: https_bench_server [port] [cert] [key]
 *   Default port: 8443
 *   Default cert: bench_cert.pem (in current directory)
 *   Default key:  bench_key.pem  (in current directory)
 *
 * Routes:
 *   GET  /ping          → 200 "pong"  (minimal response)
 *   GET  /echo?size=N   → 200 with N bytes of 'x' (payload test)
 *   POST /echo          → 200 echoing the request body
 *
 * Benchmark with wrk (HTTP/1.1 over TLS):
 *   wrk -t4 -c100 -d10s https://127.0.0.1:8443/ping
 *
 * Benchmark with h2load (HTTP/2 over TLS with ALPN):
 *   h2load -t4 -c100 -m10 -D 10 https://127.0.0.1:8443/ping
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <vector>

extern "C" {
#include <xbase/event.h>
#include <xhttp/server.h>
#include <xnet/tls.h>
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
  uint16_t port = 8443;
  const char *cert_path = "bench_cert.pem";
  const char *key_path = "bench_key.pem";

  if (argc > 1) port = (uint16_t)atoi(argv[1]);
  if (argc > 2) cert_path = argv[2];
  if (argc > 3) key_path = argv[3];

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

  xTlsConf tls = {};
  tls.cert = cert_path;
  tls.key = key_path;
  tls.skip_verify = 1; // No client cert required (one-way TLS for benchmarking)

  xErrno err = xHttpServerListenTls(server, "0.0.0.0", port, &tls);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to listen TLS on port %u (err=%d)\n", port, err);
    fprintf(stderr, "Make sure cert and key files exist:\n");
    fprintf(stderr, "  cert: %s\n", cert_path);
    fprintf(stderr, "  key:  %s\n", key_path);
    fprintf(stderr, "Generate with:\n");
    fprintf(stderr, "  openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
                    "-days 365 -nodes -subj '/CN=localhost'\n",
            key_path, cert_path);
    xHttpServerDestroy(server);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  fprintf(stdout, "HTTPS bench server listening on 0.0.0.0:%u\n", port);
  fprintf(stdout, "Routes:\n");
  fprintf(stdout, "  GET  /ping        → 200 \"pong\"\n");
  fprintf(stdout, "  GET  /echo?size=N → 200 with N bytes\n");
  fprintf(stdout, "  POST /echo        → 200 echo body\n");
  fprintf(stdout, "TLS cert: %s\n", cert_path);
  fprintf(stdout, "TLS key:  %s\n", key_path);
  fprintf(stdout, "Protocol: HTTPS (TLS + ALPN h2/http1.1)\n");
  fflush(stdout);

  xEventLoopRun(g_loop);

  xHttpServerDestroy(server);
  xEventLoopDestroy(g_loop);
  return 0;
}
