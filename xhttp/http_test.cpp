/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http_test.cpp - Integration tests: xhttp client ↔ server
 *
 * Spins up a real xHttpServer and uses xHttpClient to talk to it,
 * exercising the full request/response path over HTTP/1.1 and HTTP/2.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <xhttp/client.h>
#include <xhttp/server.h>
}

#include "server_test_helper.h"

/* ───────────────────── Helpers ───────────────────── */

static void pump_until_bool(xEventLoop loop, std::atomic<bool> &flag,
                            int max_ms = 5000) {
  for (int elapsed = 0;
       elapsed < max_ms && !flag.load(std::memory_order_acquire);
       elapsed += 10) {
    xEventWait(loop, 10);
  }
}

/* ───────────────────── Response context ───────────────────── */

struct RespCtx {
  std::atomic<bool> done{false};
  long              status_code{0};
  int               curl_code{-1};
  std::string       body;
  std::string       headers;
};

static void on_resp(const xHttpResponse *resp, void *arg) {
  auto *ctx = static_cast<RespCtx *>(arg);
  ctx->status_code = resp->status_code;
  ctx->curl_code   = resp->curl_code;
  if (resp->body && resp->body_len > 0)
    ctx->body.assign(resp->body, resp->body_len);
  if (resp->headers && resp->headers_len > 0)
    ctx->headers.assign(resp->headers, resp->headers_len);
  ctx->done.store(true, std::memory_order_release);
}

/* ───────────────────── SSE context ───────────────────── */

struct SseTestCtx {
  std::vector<std::string> events;
  std::vector<std::string> data;
  std::atomic<int>         event_count{0};
  std::atomic<bool>        done{false};
  int                      done_curl_code{-1};
};

static int on_sse_ev(const xSseEvent *ev, void *arg) {
  auto *ctx = static_cast<SseTestCtx *>(arg);
  ctx->events.emplace_back(ev->event ? ev->event : "");
  ctx->data.emplace_back(ev->data ? ev->data : "");
  ctx->event_count.fetch_add(1, std::memory_order_release);
  return 0;
}

static void on_sse_end(int curl_code, void *arg) {
  auto *ctx = static_cast<SseTestCtx *>(arg);
  ctx->done_curl_code = curl_code;
  ctx->done.store(true, std::memory_order_release);
}

/* ───────────────────── Server handlers ───────────────────── */

static void hello_handler(xHttpResponseWriter w, const xHttpRequest *req,
                           void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  xHttpResponseSend(w, "hello", 5);
}

static void echo_body_handler(xHttpResponseWriter w, const xHttpRequest *req,
                               void *arg) {
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "application/octet-stream");
  xHttpResponseSend(w, req->body, req->body_len);
}

static void echo_header_handler(xHttpResponseWriter w,
                                 const xHttpRequest *req, void *arg) {
  (void)arg;
  /* Echo back the raw request headers as the response body */
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  xHttpResponseSend(w, req->headers, req->headers_len);
}

static void sse_handler(xHttpResponseWriter w, const xHttpRequest *req,
                         void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/event-stream");
  xHttpResponseSetHeader(w, "Cache-Control", "no-cache");

  xHttpResponseWrite(w, "data: alpha\n\n", 13);
  xHttpResponseWrite(w, "event: custom\ndata: beta\n\n", 26);
  xHttpResponseWrite(w, "data: gamma\n\n", 13);
  xHttpResponseEnd(w);
}

/* ───────────────────── Fixture ───────────────────── */

class IntegrationTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpServer server = nullptr;
  xHttpClient client = nullptr;
  uint16_t    port   = 0;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);

    server = xHttpServerCreate(loop);
    ASSERT_NE(server, nullptr);

    client = xHttpClientCreate(loop);
    ASSERT_NE(client, nullptr);

    port = find_free_port();
    ASSERT_NE(port, 0) << "Could not find a free port";
  }

  void TearDown() override {
    if (client) xHttpClientDestroy(client);
    if (server) xHttpServerDestroy(server);
    if (loop)   xEventLoopDestroy(loop);
  }

  void listen_and_pump() {
    xErrno err = xHttpServerListen(server, "127.0.0.1", port);
    ASSERT_EQ(err, xErrno_Ok) << "Failed to listen on port " << port;
    pump_loop(loop, 20);
  }

  std::string make_url(const char *path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }
};

/* ───────────────────── H1 GET ───────────────────── */

TEST_F(IntegrationTest, H1Get) {
  xHttpServerRoute(server, "GET", "/hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx ctx;
  std::string url = make_url("/hello");
  xErrno err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── H1 POST with body echo ───────────────────── */

TEST_F(IntegrationTest, H1PostEcho) {
  xHttpServerRoute(server, "POST", "/echo", echo_body_handler, nullptr);
  listen_and_pump();

  RespCtx ctx;
  std::string url = make_url("/echo");
  const char *body = "{\"msg\":\"integration\"}";
  xErrno err = xHttpClientPost(client, url.c_str(), body, strlen(body),
                                on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, body);
}

/* ───────────────────── H1 Do with custom headers ───────────────────── */

TEST_F(IntegrationTest, H1DoCustomHeaders) {
  xHttpServerRoute(server, "GET", "/headers", echo_header_handler, nullptr);
  listen_and_pump();

  RespCtx ctx;
  std::string url = make_url("/headers");

  const char *hdrs[] = {"X-Test-Key: test-value-123", NULL};
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = url.c_str();
  config.method  = xHttpMethod_GET;
  config.headers = hdrs;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* The echoed headers should contain our custom header */
  EXPECT_NE(ctx.body.find("X-Test-Key"), std::string::npos);
  EXPECT_NE(ctx.body.find("test-value-123"), std::string::npos);
}

/* ───────────────────── H1 404 Not Found ───────────────────── */

TEST_F(IntegrationTest, H1NotFound) {
  xHttpServerRoute(server, "GET", "/exists", hello_handler, nullptr);
  listen_and_pump();

  RespCtx ctx;
  std::string url = make_url("/nonexistent");
  xErrno err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 404);
}

/* ───────────────────── H2C Prior Knowledge GET ───────────────────── */

TEST_F(IntegrationTest, H2cGet) {
  xHttpServerRoute(server, "GET", "/hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx ctx;
  std::string url = make_url("/hello");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── H2C POST with body echo ───────────────────── */

TEST_F(IntegrationTest, H2cPostEcho) {
  xHttpServerRoute(server, "POST", "/echo", echo_body_handler, nullptr);
  listen_and_pump();

  RespCtx ctx;
  std::string url = make_url("/echo");
  const char *body = "h2c-body-test";

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_POST;
  config.body         = body;
  config.body_len     = strlen(body);
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C POST request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, body);
}

/* ───────────────────── Client default HTTP version ───────────────────── */

TEST_F(IntegrationTest, ClientDefaultH2c) {
  xHttpServerRoute(server, "GET", "/hello", hello_handler, nullptr);
  listen_and_pump();

  /* Set client-level default to H2C */
  xHttpClientSetHttpVersion(client, xHttpVersion_H2C);

  RespCtx ctx;
  std::string url = make_url("/hello");

  /* Use convenience API — should inherit client default H2C */
  xErrno err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request with default H2C timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── SSE over H1 ───────────────────── */

TEST_F(IntegrationTest, SseOverH1) {
  xHttpServerRoute(server, "GET", "/events", sse_handler, nullptr);
  listen_and_pump();

  SseTestCtx ctx;
  std::string url = make_url("/events");
  xErrno err = xHttpClientGetSse(client, url.c_str(),
                                  on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE stream did not finish in time";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 3);
  EXPECT_EQ(ctx.events[0], "message");
  EXPECT_EQ(ctx.data[0], "alpha");
  EXPECT_EQ(ctx.events[1], "custom");
  EXPECT_EQ(ctx.data[1], "beta");
  EXPECT_EQ(ctx.events[2], "message");
  EXPECT_EQ(ctx.data[2], "gamma");
}

/* ───────────────────── SSE over H2C ───────────────────── */

TEST_F(IntegrationTest, SseOverH2c) {
  GTEST_SKIP() << "Server-side H2 streaming (write_data) not yet implemented";
  xHttpServerRoute(server, "GET", "/events", sse_handler, nullptr);
  listen_and_pump();

  SseTestCtx ctx;
  std::string url = make_url("/events");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDoSse(client, &config,
                                 on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE/H2C stream did not finish in time";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 3);
  EXPECT_EQ(ctx.data[0], "alpha");
  EXPECT_EQ(ctx.data[1], "beta");
  EXPECT_EQ(ctx.data[2], "gamma");
}

/* ───────────────────── Concurrent H1 + H2C requests ───────────────────── */

TEST_F(IntegrationTest, ConcurrentH1AndH2c) {
  xHttpServerRoute(server, "GET", "/hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx ctx_h1, ctx_h2c;
  std::string url = make_url("/hello");

  /* H1 request */
  xErrno err1 = xHttpClientGet(client, url.c_str(), on_resp, &ctx_h1);
  ASSERT_EQ(err1, xErrno_Ok);

  /* H2C request */
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;

  xErrno err2 = xHttpClientDo(client, &config, on_resp, &ctx_h2c);
  ASSERT_EQ(err2, xErrno_Ok);

  /* Pump until both complete */
  for (int elapsed = 0; elapsed < 5000; elapsed += 10) {
    if (ctx_h1.done.load(std::memory_order_acquire) &&
        ctx_h2c.done.load(std::memory_order_acquire))
      break;
    xEventWait(loop, 10);
  }

  ASSERT_TRUE(ctx_h1.done.load()) << "H1 request timed out";
  EXPECT_EQ(ctx_h1.curl_code, 0);
  EXPECT_EQ(ctx_h1.status_code, 200);
  EXPECT_EQ(ctx_h1.body, "hello");

  ASSERT_TRUE(ctx_h2c.done.load()) << "H2C request timed out";
  EXPECT_EQ(ctx_h2c.curl_code, 0);
  EXPECT_EQ(ctx_h2c.status_code, 200);
  EXPECT_EQ(ctx_h2c.body, "hello");
}
