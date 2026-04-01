/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * curl_test.cpp - Unit tests for xCurlMulti (xbase curl integration)
 *
 * Tests are grouped into:
 *   1. Lifecycle  - create / destroy
 *   2. Args       - NULL / invalid argument validation
 *   3. HTTP       - real GET / POST against httpbin.org (network required)
 *   4. Concurrent - multiple in-flight requests
 *   5. Error      - DNS failure / invalid host
 *
 * Network tests are skipped automatically when the host has no connectivity
 * (detected by a pre-test DNS probe via testing::Environment).
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>

#include <curl/curl.h>
extern "C" {
#include "curl.h"
#include <xbase/event.h>
}

/* ───────────────────── Network probe ───────────────────── */

static bool g_has_network = false;

class CurlGlobalEnv : public ::testing::Environment {
 public:
  void SetUp() override {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURL *probe = curl_easy_init();
    if (probe) {
      curl_easy_setopt(probe, CURLOPT_URL, "https://httpbin.org/get");
      curl_easy_setopt(probe, CURLOPT_NOBODY, 1L);
      curl_easy_setopt(probe, CURLOPT_TIMEOUT_MS, 3000L);
      curl_easy_setopt(probe, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
      g_has_network = (curl_easy_perform(probe) == CURLE_OK);
      curl_easy_cleanup(probe);
    }
  }

  void TearDown() override { curl_global_cleanup(); }
};

/* ───────────────────── Helpers ───────────────────── */

/* Pump the event loop for up to max_ms milliseconds or until pred() is true. */
static bool pump_until(xEventLoop loop, std::function<bool()> pred,
                       int max_ms = 10000) {
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(max_ms);
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    xEventWait(loop, 50);
  }
  return true;
}

struct DoneCtx {
  std::atomic<bool> called{false};
  long              http_code{0};
  int               err_code{0};
  std::string       err_msg;
};

static void done_cb(void *arg, long http_code, int err_code,
                    const char *err_msg) {
  auto *ctx      = static_cast<DoneCtx *>(arg);
  ctx->http_code = http_code;
  ctx->err_code  = err_code;
  ctx->err_msg   = err_msg ? err_msg : "";
  ctx->called.store(true, std::memory_order_release);
}

/* ───────────────────── Test fixture ───────────────────── */

class CurlTest : public ::testing::Test {
 protected:
  xEventLoop loop{nullptr};
  xCurlMulti multi{nullptr};

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    multi = xCurlMultiNew(loop);
    ASSERT_NE(multi, nullptr);
  }

  void TearDown() override {
    xCurlMultiDestroy(multi);
    xEventLoopDestroy(loop);
  }
};

/* ───────────────────── 1. Lifecycle ───────────────────── */

TEST(CurlLifecycle, CreateDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xCurlMulti m = xCurlMultiNew(loop);
  EXPECT_NE(m, nullptr);

  xCurlMultiDestroy(m);
  xEventLoopDestroy(loop);
}

TEST(CurlLifecycle, DestroyNullIsNoop) {
  xCurlMultiDestroy(nullptr); /* must not crash */
}

/* ───────────────────── 2. Argument validation ───────────────────── */

TEST(CurlArgs, NewNullLoopReturnsNull) {
  EXPECT_EQ(xCurlMultiNew(nullptr), nullptr);
}

TEST(CurlArgs, GetNullMultiReturnsError) {
  EXPECT_EQ(xCurlMultiGet(nullptr, "https://example.com", done_cb, nullptr),
            xErrno_InvalidArg);
}

TEST(CurlArgs, GetNullUrlReturnsError) {
  xEventLoop loop = xEventLoopCreate();
  xCurlMulti m    = xCurlMultiNew(loop);

  EXPECT_EQ(xCurlMultiGet(m, nullptr, done_cb, nullptr), xErrno_InvalidArg);

  xCurlMultiDestroy(m);
  xEventLoopDestroy(loop);
}

TEST(CurlArgs, GetNullCallbackReturnsError) {
  xEventLoop loop = xEventLoopCreate();
  xCurlMulti m    = xCurlMultiNew(loop);

  EXPECT_EQ(xCurlMultiGet(m, "https://example.com", nullptr, nullptr),
            xErrno_InvalidArg);

  xCurlMultiDestroy(m);
  xEventLoopDestroy(loop);
}

TEST(CurlArgs, PostNullMultiReturnsError) {
  EXPECT_EQ(xCurlMultiPost(nullptr, "https://example.com", nullptr, 0,
                            done_cb, nullptr),
            xErrno_InvalidArg);
}

TEST(CurlArgs, PostNullUrlReturnsError) {
  xEventLoop loop = xEventLoopCreate();
  xCurlMulti m    = xCurlMultiNew(loop);

  EXPECT_EQ(xCurlMultiPost(m, nullptr, nullptr, 0, done_cb, nullptr),
            xErrno_InvalidArg);

  xCurlMultiDestroy(m);
  xEventLoopDestroy(loop);
}

TEST(CurlArgs, PostNullCallbackReturnsError) {
  xEventLoop loop = xEventLoopCreate();
  xCurlMulti m    = xCurlMultiNew(loop);

  EXPECT_EQ(xCurlMultiPost(m, "https://example.com", nullptr, 0, nullptr,
                            nullptr),
            xErrno_InvalidArg);

  xCurlMultiDestroy(m);
  xEventLoopDestroy(loop);
}

/* ───────────────────── 3. HTTP (network required) ───────────────────── */

class CurlNetworkTest : public CurlTest {
 protected:
  void SetUp() override {
    if (!g_has_network) GTEST_SKIP() << "No network connectivity";
    CurlTest::SetUp();
  }
};

TEST_F(CurlNetworkTest, GetReturns200) {
  DoneCtx ctx;
  ASSERT_EQ(xCurlMultiGet(multi, "https://httpbin.org/get", done_cb, &ctx),
            xErrno_Ok);

  ASSERT_TRUE(pump_until(loop, [&] {
    return ctx.called.load(std::memory_order_acquire);
  }));

  EXPECT_EQ(ctx.err_code, CURLE_OK);
  EXPECT_EQ(ctx.http_code, 200L);
}

TEST_F(CurlNetworkTest, PostReturns200) {
  DoneCtx ctx;
  const char *body = "{\"key\":\"value\"}";
  ASSERT_EQ(xCurlMultiPost(multi, "https://httpbin.org/post", body,
                            strlen(body), done_cb, &ctx),
            xErrno_Ok);

  ASSERT_TRUE(pump_until(loop, [&] {
    return ctx.called.load(std::memory_order_acquire);
  }));

  EXPECT_EQ(ctx.err_code, CURLE_OK);
  EXPECT_EQ(ctx.http_code, 200L);
}

TEST_F(CurlNetworkTest, Get404ReturnsHttpCode) {
  DoneCtx ctx;
  ASSERT_EQ(xCurlMultiGet(multi, "https://httpbin.org/status/404", done_cb,
                           &ctx),
            xErrno_Ok);

  ASSERT_TRUE(pump_until(loop, [&] {
    return ctx.called.load(std::memory_order_acquire);
  }));

  EXPECT_EQ(ctx.err_code, CURLE_OK);
  EXPECT_EQ(ctx.http_code, 404L);
}

TEST_F(CurlNetworkTest, GetCallbackReceivesUserArg) {
  std::atomic<void *> received{nullptr};

  auto cb = [](void *arg, long, int, const char *) {
    static_cast<std::atomic<void *> *>(arg)->store(arg,
                                                    std::memory_order_release);
  };

  ASSERT_EQ(xCurlMultiGet(multi, "https://httpbin.org/get",
                           (xCurlDoneFunc)cb, &received),
            xErrno_Ok);

  ASSERT_TRUE(pump_until(loop, [&] {
    return received.load(std::memory_order_acquire) != nullptr;
  }));

  EXPECT_EQ(received.load(), static_cast<void *>(&received));
}

/* ───────────────────── 4. Concurrent requests ───────────────────── */

TEST_F(CurlNetworkTest, MultipleConcurrentGets) {
  constexpr int N = 5;
  DoneCtx       ctxs[N];

  for (int i = 0; i < N; i++) {
    ASSERT_EQ(
        xCurlMultiGet(multi, "https://httpbin.org/get", done_cb, &ctxs[i]),
        xErrno_Ok);
  }

  ASSERT_TRUE(pump_until(
      loop,
      [&] {
        for (int i = 0; i < N; i++)
          if (!ctxs[i].called.load(std::memory_order_acquire)) return false;
        return true;
      },
      30000));

  for (int i = 0; i < N; i++) {
    EXPECT_EQ(ctxs[i].err_code, CURLE_OK) << "request " << i;
    EXPECT_EQ(ctxs[i].http_code, 200L) << "request " << i;
  }
}

/* ───────────────────── 5. Error handling ───────────────────── */

TEST_F(CurlNetworkTest, InvalidHostReturnsError) {
  DoneCtx ctx;
  ASSERT_EQ(xCurlMultiGet(multi, "http://this-host-does-not-exist.invalid/",
                           done_cb, &ctx),
            xErrno_Ok);

  ASSERT_TRUE(pump_until(loop, [&] {
    return ctx.called.load(std::memory_order_acquire);
  }, 10000));

  EXPECT_NE(ctx.err_code, CURLE_OK);
}

/* ───────────────────── Environment registration ───────────────────── */

/* Called before RUN_ALL_TESTS() by gtest_main. */
static void register_env() __attribute__((constructor));
static void register_env() {
  ::testing::AddGlobalTestEnvironment(new CurlGlobalEnv());
}
