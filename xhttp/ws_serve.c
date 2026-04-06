/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ws_serve.c - WebSocket-only server convenience function
 */

#include <xhttp/server.h>
#include <xhttp/ws.h>

#include "server_private.h"

#include <stdlib.h>

/* ── Internal state for the catch-all upgrade handler ──── */

typedef struct {
  xWsCallbacks callbacks;
  void        *user_arg;
} xWsServeCtx;

static void ws_serve_handler(xHttpResponseWriter w,
                             const xHttpRequest *req,
                             void *arg) {
  xWsServeCtx *ctx = (xWsServeCtx *)arg;
  xWsUpgrade(w, req, &ctx->callbacks, ctx->user_arg);
}

/* ── Public API ────────────────────────────────────────── */

xHttpServer xWsServe(xEventLoop loop,
                     const char *host,
                     uint16_t port,
                     const xWsCallbacks *callbacks,
                     void *arg) {
  if (!loop || !callbacks) return NULL;

  xHttpServer server = xHttpServerCreate(loop);
  if (!server) return NULL;

  /* Allocate context that outlives this function call.
   * Freed when the server is destroyed (the route's arg
   * is opaque to the server, so we rely on the caller
   * to destroy the server via xHttpServerDestroy). */
  xWsServeCtx *ctx =
    (xWsServeCtx *)calloc(1, sizeof(xWsServeCtx));
  if (!ctx) {
    xHttpServerDestroy(server);
    return NULL;
  }

  ctx->callbacks = *callbacks;
  ctx->user_arg  = arg;

  /* Attach ctx to the server so it is freed automatically
   * when xHttpServerDestroy() is called. */
  struct xHttpServer_ *s = (struct xHttpServer_ *)server;
  s->aux_data = ctx;
  s->aux_free = free;

  xErrno err = xHttpServerRoute(
    server, "GET /", ws_serve_handler, ctx);
  if (err != xErrno_Ok) {
    s->aux_data = NULL;
    s->aux_free = NULL;
    free(ctx);
    xHttpServerDestroy(server);
    return NULL;
  }

  err = xHttpServerListen(server, host, port);
  if (err != xErrno_Ok) {
    s->aux_data = NULL;
    s->aux_free = NULL;
    free(ctx);
    xHttpServerDestroy(server);
    return NULL;
  }

  return server;
}
