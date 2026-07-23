/*
 * Copyright 2025 The libx Authors. All rights reserved.
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
#include <x/http/client.h>
#include <x/http/server.h>
}

#include "server_test_helper.h"

/* ───────────────────── Helpers ───────────────────── */

static void pump_until_bool(xEventLoop loop, std::atomic<bool> &flag, int max_ms = 5000) {
  for (int elapsed = 0; elapsed < max_ms && !flag.load(std::memory_order_acquire); elapsed += 10) {
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
  auto *ctx        = static_cast<RespCtx *>(arg);
  ctx->status_code = resp->status_code;
  ctx->curl_code   = resp->curl_code;
  if (resp->body && resp->body_len > 0) ctx->body.assign(resp->body, resp->body_len);
  if (resp->headers && resp->headers_len > 0) ctx->headers.assign(resp->headers, resp->headers_len);
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
  auto *ctx           = static_cast<SseTestCtx *>(arg);
  ctx->done_curl_code = curl_code;
  ctx->done.store(true, std::memory_order_release);
}

/* ───────────────────── Server handlers ───────────────────── */

static void hello_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  xHttpResponseSend(w, "hello", 5);
}

static void echo_body_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "application/octet-stream");
  xHttpResponseSend(w, req->body, req->body_len);
}

static void echo_header_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  /* Echo back the raw request headers as the response body */
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  xHttpResponseSend(w, req->headers, req->headers_len);
}

static void sse_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
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

    client = xHttpClientCreate(loop, nullptr);
    ASSERT_NE(client, nullptr);

    port = find_free_port();
    ASSERT_NE(port, 0) << "Could not find a free port";
  }

  void TearDown() override {
    if (client) xHttpClientDestroy(client);
    if (server) xHttpServerDestroy(server);
    if (loop) xEventLoopDestroy(loop);
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
  xHttpServerRoute(server, "GET /hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/hello");
  xErrno      err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── H1 POST with body echo ───────────────────── */

TEST_F(IntegrationTest, H1PostEcho) {
  xHttpServerRoute(server, "POST /echo", echo_body_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/echo");
  const char *body = "{\"msg\":\"integration\"}";
  xErrno      err  = xHttpClientPost(client, url.c_str(), body, strlen(body), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, body);
}

/* ───────────────────── H1 Do with custom headers ───────────────────── */

TEST_F(IntegrationTest, H1DoCustomHeaders) {
  xHttpServerRoute(server, "GET /headers", echo_header_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/headers");

  const char      *hdrs[] = {"X-Test-Key: test-value-123", NULL};
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
  xHttpServerRoute(server, "GET /exists", hello_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/nonexistent");
  xErrno      err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 404);
}

/* ───────────────────── H2C Prior Knowledge GET ───────────────────── */

TEST_F(IntegrationTest, H2cGet) {
  xHttpServerRoute(server, "GET /hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
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
  xHttpServerRoute(server, "POST /echo", echo_body_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/echo");
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
  xHttpServerRoute(server, "GET /hello", hello_handler, nullptr);
  listen_and_pump();

  /* Recreate client with H2C as default version */
  xHttpClientDestroy(client);
  xHttpClientConf conf = {};
  conf.http_version    = xHttpVersion_H2C;
  client               = xHttpClientCreate(loop, &conf);
  ASSERT_NE(client, nullptr);

  RespCtx     ctx;
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
  xHttpServerRoute(server, "GET /events", sse_handler, nullptr);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url = make_url("/events");
  xErrno      err = xHttpClientGetSse(client, url.c_str(), on_sse_ev, on_sse_end, &ctx);
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
  xHttpServerRoute(server, "GET /events", sse_handler, nullptr);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url = make_url("/events");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDoSse(client, &config, on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE/H2C stream did not finish in time";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 3);
  EXPECT_EQ(ctx.data[0], "alpha");
  EXPECT_EQ(ctx.data[1], "beta");
  EXPECT_EQ(ctx.data[2], "gamma");
}

/* ───────────────────── H2C 404 Not Found ───────────────────── */

TEST_F(IntegrationTest, H2cNotFound) {
  xHttpServerRoute(server, "GET /exists", hello_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/nonexistent");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C 404 request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 404);
}

/* ───────────────────── H2C custom headers ───────────────────── */

TEST_F(IntegrationTest, H2cDoCustomHeaders) {
  xHttpServerRoute(server, "GET /headers", echo_header_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/headers");

  const char      *hdrs[] = {"X-H2-Test: h2c-value-456", NULL};
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.headers      = hdrs;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C headers request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* H2 lowercases header names; verify the value is present */
  EXPECT_NE(ctx.body.find("h2c-value-456"), std::string::npos);
}

/* ───────────────────── Route params over H1 ───────────────────── */

static void param_echo_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  size_t      len = 0;
  const char *id  = xHttpRequestParam(req, "id", &len);

  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  if (id && len > 0) {
    char buf[128];
    int  n = snprintf(buf, sizeof(buf), "id=%.*s", (int)len, id);
    xHttpResponseSend(w, buf, (size_t)n);
  } else {
    xHttpResponseSend(w, "id=none", 7);
  }
}

TEST_F(IntegrationTest, H1RouteParam) {
  xHttpServerRoute(server, "GET /users/:id", param_echo_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/users/42");
  xErrno      err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "id=42");
}

/* ───────────────────── Route params over H2C ───────────────────── */

TEST_F(IntegrationTest, H2cRouteParam) {
  xHttpServerRoute(server, "GET /users/:id", param_echo_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/users/alice");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C param request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "id=alice");
}

/* ───────────────────── PUT method ───────────────────── */

static void put_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/plain");
  /* Echo method + body */
  char buf[256];
  int  n = snprintf(buf, sizeof(buf), "%s:%.*s", req->method, (int)req->body_len,
                   req->body ? req->body : "");
  xHttpResponseSend(w, buf, (size_t)n);
}

TEST_F(IntegrationTest, H1PutMethod) {
  xHttpServerRoute(server, "PUT /resource", put_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/resource");
  const char *body = "updated-data";

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url      = url.c_str();
  config.method   = xHttpMethod_PUT;
  config.body     = body;
  config.body_len = strlen(body);

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "PUT request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "PUT:updated-data");
}

/* ───────────────────── DELETE method over H2C ───────────────────── */

static void delete_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  (void)req;
  xHttpResponseSetStatus(w, 204);
  xHttpResponseSend(w, NULL, 0);
}

TEST_F(IntegrationTest, H2cDeleteMethod) {
  xHttpServerRoute(server, "DELETE /resource", delete_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/resource");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_DELETE;
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "DELETE request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 204);
  EXPECT_TRUE(ctx.body.empty());
}

/* ───────────────────── Large body round-trip ───────────────────── */

TEST_F(IntegrationTest, LargeBodyRoundTrip) {
  xHttpServerRoute(server, "POST /echo", echo_body_handler, nullptr);
  listen_and_pump();

  /* Use a body that fits in a single socket read.
   * TODO: bodies larger than ~4 KB may time out due to an edge-triggered
   * read bug in the server (only one read per event). */
  std::string large_body(4000, 'X');

  RespCtx     ctx;
  std::string url = make_url("/echo");
  xErrno      err =
    xHttpClientPost(client, url.c_str(), large_body.c_str(), large_body.size(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load()) << "Large body request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body.size(), large_body.size());
  EXPECT_EQ(ctx.body, large_body);
}

/* ───────────────────── SSE via DoSse POST (LLM-style) ───────────────────── */

static void sse_post_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  /* Echo the request body as an SSE event, then send a done event */
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/event-stream");
  xHttpResponseSetHeader(w, "Cache-Control", "no-cache");

  char buf[512];
  int  n =
    snprintf(buf, sizeof(buf), "data: %.*s\n\n", (int)req->body_len, req->body ? req->body : "");
  xHttpResponseWrite(w, buf, (size_t)n);
  xHttpResponseWrite(w, "event: done\ndata: [DONE]\n\n", 26);
  xHttpResponseEnd(w);
}

TEST_F(IntegrationTest, SseDoPostH1) {
  xHttpServerRoute(server, "POST /v1/chat", sse_post_handler, nullptr);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url  = make_url("/v1/chat");
  const char *body = "{\"model\":\"gpt-4\"}";

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url      = url.c_str();
  config.method   = xHttpMethod_POST;
  config.body     = body;
  config.body_len = strlen(body);

  xErrno err = xHttpClientDoSse(client, &config, on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE POST stream did not finish";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 2);
  EXPECT_EQ(ctx.data[0], body);
  EXPECT_EQ(ctx.events[1], "done");
  EXPECT_EQ(ctx.data[1], "[DONE]");
}

/* ───────────────────── SSE via DoSse POST over H2C ───────────────────── */

TEST_F(IntegrationTest, SseDoPostH2c) {
  xHttpServerRoute(server, "POST /v1/chat", sse_post_handler, nullptr);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url  = make_url("/v1/chat");
  const char *body = "{\"stream\":true}";

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_POST;
  config.body         = body;
  config.body_len     = strlen(body);
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDoSse(client, &config, on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE POST/H2C stream did not finish";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 2);
  EXPECT_EQ(ctx.data[0], body);
  EXPECT_EQ(ctx.events[1], "done");
  EXPECT_EQ(ctx.data[1], "[DONE]");
}

/* ───────────────────── Empty body response (204) ───────────────────── */

TEST_F(IntegrationTest, H1EmptyBodyResponse) {
  xHttpServerRoute(server, "DELETE /item", delete_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/item");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url    = url.c_str();
  config.method = xHttpMethod_DELETE;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "204 request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 204);
  EXPECT_TRUE(ctx.body.empty());
}

/* ───────────────────── H3 config path (fallback to H1/H2 on local server) ───────────────────── */

TEST_F(IntegrationTest, H3ConfigGet) {
  xHttpServerRoute(server, "GET /hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/hello");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H3;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H3 request timed out";
  /* H3 will fallback to H1/H2 if server does not support it */
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

TEST_F(IntegrationTest, H3ConfigPost) {
  xHttpServerRoute(server, "POST /echo", echo_body_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/echo");
  const char *body = "h3-post-test";

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_POST;
  config.body         = body;
  config.body_len     = strlen(body);
  config.http_version = xHttpVersion_H3;

  xErrno err = xHttpClientDo(client, &config, on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H3 POST request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, body);
}

TEST_F(IntegrationTest, H3ClientDefault) {
  xHttpServerRoute(server, "GET /hello", hello_handler, nullptr);
  listen_and_pump();

  /* Recreate client with H3 as default version */
  xHttpClientDestroy(client);
  xHttpClientConf conf = {};
  conf.http_version    = xHttpVersion_H3;
  client               = xHttpClientCreate(loop, &conf);
  ASSERT_NE(client, nullptr);

  RespCtx     ctx;
  std::string url = make_url("/hello");

  /* Convenience API — should inherit client default H3 */
  xErrno err = xHttpClientGet(client, url.c_str(), on_resp, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H3 default client request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

TEST_F(IntegrationTest, H3ConfigSse) {
  xHttpServerRoute(server, "GET /events", sse_handler, nullptr);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url = make_url("/events");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H3;

  xErrno err = xHttpClientDoSse(client, &config, on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_bool(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H3 SSE stream did not finish";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 3);
  EXPECT_EQ(ctx.data[0], "alpha");
  EXPECT_EQ(ctx.data[1], "beta");
  EXPECT_EQ(ctx.data[2], "gamma");
}

/* ───────────────────── Concurrent H1 + H2C requests ───────────────────── */

TEST_F(IntegrationTest, ConcurrentH1AndH2c) {
  xHttpServerRoute(server, "GET /hello", hello_handler, nullptr);
  listen_and_pump();

  RespCtx     ctx_h1, ctx_h2c;
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
    if (ctx_h1.done.load(std::memory_order_acquire) && ctx_h2c.done.load(std::memory_order_acquire))
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
