/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_test.cpp - Unit tests for xhttp (async HTTP client)
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <xhttp/client.h>
}

/* Skip network-dependent tests when XKIT_SKIP_NETWORK_TESTS=1 */
static bool skip_network_tests() {
  const char *v = std::getenv("XKIT_SKIP_NETWORK_TESTS");
  return v && std::string(v) == "1";
}

#define SKIP_IF_NO_NETWORK() \
  if (skip_network_tests()) GTEST_SKIP() << "Network tests disabled"

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

/**
 * @brief Pump the event loop until a flag becomes true or timeout.
 */
static void pump_until(xEventLoop loop, std::atomic<bool> &flag,
                       int max_ms = 5000) {
  for (int elapsed = 0;
       elapsed < max_ms && !flag.load(std::memory_order_acquire);
       elapsed += 10) {
    xEventWait(loop, 10);
  }
}

static void pump_until_count(xEventLoop loop, std::atomic<int> &count,
                             int target, int max_ms = 10000) {
  for (int elapsed = 0;
       elapsed < max_ms && count.load(std::memory_order_acquire) < target;
       elapsed += 10) {
    xEventWait(loop, 10);
  }
}

/* ───────────────────── Fixture ───────────────────── */

class HttpClientTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpClient client = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);

    client = xHttpClientCreate(loop, nullptr);
    ASSERT_NE(client, nullptr);
  }

  void TearDown() override {
    if (client) xHttpClientDestroy(client);
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Create / Destroy ───────────────────── */

TEST(HttpClientLifecycle, CreateAndDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xHttpClient c = xHttpClientCreate(loop, nullptr);
  ASSERT_NE(c, nullptr);

  /* Destroy immediately — no requests in flight */
  xHttpClientDestroy(c);
  xEventLoopDestroy(loop);
}

TEST(HttpClientLifecycle, CreateWithNullLoopReturnsNull) {
  xHttpClient c = xHttpClientCreate(nullptr, nullptr);
  EXPECT_EQ(c, nullptr);
}

/* ───────────────────── GET request ───────────────────── */

struct ResponseCtx {
  std::atomic<bool> done{false};
  long              status_code{0};
  int               curl_code{-1};
  std::string       body;
  std::string       headers;
  std::string       curl_error;
};

static void on_response(const xHttpResponse *resp, void *arg) {
  auto *ctx        = static_cast<ResponseCtx *>(arg);
  ctx->status_code = resp->status_code;
  ctx->curl_code   = resp->curl_code;
  if (resp->body && resp->body_len > 0)
    ctx->body.assign(resp->body, resp->body_len);
  if (resp->headers && resp->headers_len > 0)
    ctx->headers.assign(resp->headers, resp->headers_len);
  if (resp->curl_error) ctx->curl_error = resp->curl_error;
  ctx->done.store(true, std::memory_order_release);
}

TEST_F(HttpClientTest, GetRequest) {
  SKIP_IF_NO_NETWORK();
  ResponseCtx ctx;

  xErrno err =
    xHttpClientGet(client, "https://httpbin.org/get", on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0); /* CURLE_OK */
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_FALSE(ctx.body.empty());
  EXPECT_FALSE(ctx.headers.empty());
}

/* ───────────────────── POST request ───────────────────── */

TEST_F(HttpClientTest, PostRequest) {
  SKIP_IF_NO_NETWORK();
  ResponseCtx ctx;
  const char *body = "{\"hello\":\"world\"}";

  xErrno err = xHttpClientPost(client, "https://httpbin.org/post", body,
                               strlen(body), on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* httpbin echoes the posted data back */
  EXPECT_NE(ctx.body.find("hello"), std::string::npos);
}

/* ───────────────────── Multiple concurrent requests ───────────────────── */

TEST_F(HttpClientTest, ConcurrentRequests) {
  SKIP_IF_NO_NETWORK();
  constexpr int    N = 3;
  std::atomic<int> done_count{0};

  struct MultiCtx {
    std::atomic<int> *counter;
    long              status_code{0};
  };

  std::vector<MultiCtx> ctxs(N);
  for (auto &c : ctxs)
    c.counter = &done_count;

  auto multi_cb = [](const xHttpResponse *resp, void *arg) {
    auto *ctx        = static_cast<MultiCtx *>(arg);
    ctx->status_code = resp->status_code;
    ctx->counter->fetch_add(1, std::memory_order_release);
  };

  for (int i = 0; i < N; i++) {
    xErrno err =
      xHttpClientGet(client, "https://httpbin.org/get", multi_cb, &ctxs[i]);
    ASSERT_EQ(err, xErrno_Ok);
  }

  pump_until_count(loop, done_count, N, 15000);

  /*
   * Destroy client before ctxs goes out of scope. If any requests are
   * still in-flight, xHttpClientDestroy will invoke their callbacks
   * which reference ctxs elements — those must still be alive.
   */
  xHttpClientDestroy(client);
  client = nullptr; /* prevent double-destroy in TearDown */

  EXPECT_EQ(done_count.load(), N);
  for (int i = 0; i < N; i++) {
    EXPECT_EQ(ctxs[i].status_code, 200);
  }
}

/* ───────────────────── Request failure (invalid URL) ───────────────────── */

TEST_F(HttpClientTest, InvalidUrlFails) {
  SKIP_IF_NO_NETWORK();
  ResponseCtx ctx;

  xErrno err =
    xHttpClientGet(client, "http://invalid.host.that.does.not.exist.example/",
                   on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok); /* submission succeeds, failure is async */

  pump_until(loop, ctx.done, 15000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_NE(ctx.curl_code, 0); /* should be a curl error */
  EXPECT_EQ(ctx.status_code, 0);
}

/* ───────────────────── Destroy with in-flight requests ─────────────────────
 */

TEST(HttpClientLifecycle, DestroyWithInflightRequests) {
  SKIP_IF_NO_NETWORK();
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xHttpClient c = xHttpClientCreate(loop, nullptr);
  ASSERT_NE(c, nullptr);

  std::atomic<bool> cb_called{false};

  auto cb = [](const xHttpResponse * /* resp */, void *arg) {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    flag->store(true, std::memory_order_release);
  };

  /* Submit a request to a slow endpoint */
  xErrno err =
    xHttpClientGet(c, "https://httpbin.org/delay/10", cb, &cb_called);
  ASSERT_EQ(err, xErrno_Ok);

  /* Pump briefly to let curl start the connection */
  for (int i = 0; i < 10; i++) {
    xEventWait(loop, 10);
  }

  /* Destroy while request is in flight — should not crash */
  xHttpClientDestroy(c);
  xEventLoopDestroy(loop);

  /* The callback may or may not have been called depending on timing,
   * but we must not crash. */
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(HttpClientTest, GetNullUrlReturnsError) {
  EXPECT_EQ(xHttpClientGet(client, nullptr, on_response, nullptr),
            xErrno_Unknown);
}

TEST_F(HttpClientTest, GetNullClientReturnsError) {
  EXPECT_EQ(
    xHttpClientGet(nullptr, "https://example.com", on_response, nullptr),
    xErrno_Unknown);
}

TEST_F(HttpClientTest, PostNullUrlReturnsError) {
  EXPECT_EQ(xHttpClientPost(client, nullptr, "body", 4, on_response, nullptr),
            xErrno_Unknown);
}

/* ───────────────────── Generic Do request ───────────────────── */

TEST_F(HttpClientTest, DoGetRequest) {
  SKIP_IF_NO_NETWORK();
  ResponseCtx ctx;

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url    = "https://httpbin.org/get";
  config.method = xHttpMethod_GET;

  xErrno err = xHttpClientDo(client, &config, on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
}

TEST_F(HttpClientTest, DoWithCustomHeaders) {
  SKIP_IF_NO_NETWORK();
  ResponseCtx ctx;

  const char *hdrs[] = {"X-Custom-Header: test-value", NULL};

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = "https://httpbin.org/headers";
  config.method  = xHttpMethod_GET;
  config.headers = hdrs;

  xErrno err = xHttpClientDo(client, &config, on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.status_code, 200);
  /* httpbin echoes headers back — verify our custom header is present */
  EXPECT_NE(ctx.body.find("X-Custom-Header"), std::string::npos);
}

TEST_F(HttpClientTest, DoWithTimeout) {
  SKIP_IF_NO_NETWORK();
  ResponseCtx ctx;

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url        = "https://httpbin.org/delay/10";
  config.method     = xHttpMethod_GET;
  config.timeout_ms = 1000; /* 1 second timeout, endpoint delays 10s */

  xErrno err = xHttpClientDo(client, &config, on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out waiting for callback";
  EXPECT_NE(ctx.curl_code, 0); /* should have timed out */
  EXPECT_EQ(ctx.status_code, 0);
}

TEST_F(HttpClientTest, DoNullConfigReturnsError) {
  EXPECT_EQ(xHttpClientDo(client, nullptr, on_response, nullptr),
            xErrno_Unknown);
}
