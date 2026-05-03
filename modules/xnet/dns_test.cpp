/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_test.cpp - Unit tests for xnet DNS async resolution
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

extern "C" {
#include <xnet/compat.h>
#include <xnet/dns.h>
}

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

static void sleep_ms(int n) {
  std::this_thread::sleep_for(ms(n));
}

/* ───────────────────── Fixture ───────────────────── */

class DnsTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    xnet_init();
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
  }

  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }

  /**
   * Run the event loop in a background thread and stop it once the
   * `done` flag becomes true (or after a timeout).
   */
  void RunUntilDone(std::atomic<bool> &done, int timeout_ms = 5000) {
    std::thread runner([&]() { xEventLoopRun(loop); });

    auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      sleep_ms(10);
    }

    xEventLoopStop(loop);
    runner.join();
  }
};

/* ───────────────────── Callback context ───────────────────── */

struct DnsCtx {
  std::atomic<bool> called{false};
  xErrno            error{xErrno_Unknown};
  int               addr_count{0};
  int               first_family{0};
  std::thread::id   callback_thread{};
  xDnsResult       *result{nullptr}; /* caller frees */
};

static void dns_callback(xDnsResult *result, void *arg) {
  auto *ctx            = static_cast<DnsCtx *>(arg);
  ctx->callback_thread = std::this_thread::get_id();
  ctx->error           = result->error;
  ctx->result          = result;

  int count = 0;
  for (xDnsAddr *a = result->addrs; a; a = a->next) {
    if (count == 0) ctx->first_family = a->family;
    count++;
  }
  ctx->addr_count = count;
  ctx->called.store(true, std::memory_order_release);
}

/* ───────────────────── Successful resolution ───────────────────── */

TEST_F(DnsTest, ResolveLocalhost) {
  DnsCtx ctx;

  xDnsQuery q = xDnsResolve(loop, "localhost", NULL, NULL, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());
  EXPECT_EQ(ctx.error, xErrno_Ok);
  EXPECT_GE(ctx.addr_count, 1);

  xDnsResultFree(ctx.result);
}

/* ───────────────────── Non-existent domain ───────────────────── */

TEST_F(DnsTest, ResolveNonExistentDomain) {
  DnsCtx ctx;

  xDnsQuery q = xDnsResolve(loop, "this.domain.does.not.exist.invalid", NULL,
                            NULL, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());
  EXPECT_EQ(ctx.error, xErrno_DnsNotFound);
  EXPECT_EQ(ctx.addr_count, 0);

  xDnsResultFree(ctx.result);
}

/* ───────────────────── IPv4-only filter ───────────────────── */

TEST_F(DnsTest, ResolveIPv4Only) {
  DnsCtx ctx;

  struct addrinfo hints = {};
  hints.ai_family       = AF_INET;
  hints.ai_socktype     = SOCK_STREAM;

  xDnsQuery q =
    xDnsResolve(loop, "localhost", NULL, &hints, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());
  EXPECT_EQ(ctx.error, xErrno_Ok);
  EXPECT_GE(ctx.addr_count, 1);

  /* All addresses should be IPv4 */
  for (xDnsAddr *a = ctx.result->addrs; a; a = a->next) {
    EXPECT_EQ(a->family, AF_INET);
  }

  xDnsResultFree(ctx.result);
}

/* ───────────────────── IPv6-only filter ───────────────────── */

TEST_F(DnsTest, ResolveIPv6Only) {
  DnsCtx ctx;

  struct addrinfo hints = {};
  hints.ai_family       = AF_INET6;
  hints.ai_socktype     = SOCK_STREAM;

  xDnsQuery q =
    xDnsResolve(loop, "localhost", NULL, &hints, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());

  if (ctx.error == xErrno_Ok) {
    /* All addresses should be IPv6 */
    for (xDnsAddr *a = ctx.result->addrs; a; a = a->next) {
      EXPECT_EQ(a->family, AF_INET6);
    }
  }
  /* Some systems may not have IPv6 configured for localhost, that's OK */

  xDnsResultFree(ctx.result);
}

/* ───────────────────── NULL parameter validation ───────────────────── */

TEST_F(DnsTest, NullLoopReturnsNull) {
  xDnsQuery q = xDnsResolve(NULL, "localhost", NULL, NULL, dns_callback, NULL);
  EXPECT_EQ(q, nullptr);
}

TEST_F(DnsTest, NullHostnameReturnsNull) {
  xDnsQuery q = xDnsResolve(loop, NULL, NULL, NULL, dns_callback, NULL);
  EXPECT_EQ(q, nullptr);
}

TEST_F(DnsTest, EmptyHostnameReturnsNull) {
  xDnsQuery q = xDnsResolve(loop, "", NULL, NULL, dns_callback, NULL);
  EXPECT_EQ(q, nullptr);
}

TEST_F(DnsTest, NullCallbackReturnsNull) {
  xDnsQuery q = xDnsResolve(loop, "localhost", NULL, NULL, NULL, NULL);
  EXPECT_EQ(q, nullptr);
}

/* ───────────────────── Cancel query ───────────────────── */

TEST_F(DnsTest, CancelPreventsCallback) {
  std::atomic<bool> called{false};

  auto cancel_cb = [](xDnsResult *result, void *arg) {
    auto *flag = static_cast<std::atomic<bool> *>(arg);
    flag->store(true, std::memory_order_release);
    xDnsResultFree(result);
  };

  xDnsQuery q = xDnsResolve(loop, "localhost", NULL, NULL, cancel_cb, &called);
  ASSERT_NE(q, nullptr);

  /* Cancel immediately */
  xDnsCancel(loop, q);

  /* Pump the event loop to let the done callback fire (or not) */
  for (int i = 0; i < 100; i++) {
    xEventWait(loop, 10);
  }

  /* The callback should NOT have been invoked */
  EXPECT_FALSE(called.load());
}

/* ───────────────────── xDnsCancel(NULL) is safe ───────────────────── */

TEST_F(DnsTest, CancelNullIsSafe) {
  xDnsCancel(loop, NULL); /* should not crash */
}

/* ───────────────────── xDnsResultFree(NULL) is safe ───────────────────── */

TEST_F(DnsTest, ResultFreeNullIsSafe) {
  xDnsResultFree(NULL); /* should not crash */
}

/* ───────────────────── Callback runs on event loop thread ───────────────── */

TEST_F(DnsTest, CallbackOnEventLoopThread) {
  DnsCtx ctx;

  xDnsQuery q = xDnsResolve(loop, "localhost", NULL, NULL, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  /* Run the event loop on the current thread so we can compare thread IDs */
  std::thread::id loop_thread = std::this_thread::get_id();

  /* Use a timer to stop the loop after the DNS callback fires */
  xEventLoopTimerAfter(
    loop, [](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop,
    2000);

  /* Pump until callback fires or timer stops us */
  for (int i = 0; i < 500 && !ctx.called.load(std::memory_order_acquire); i++) {
    xEventWait(loop, 10);
  }

  EXPECT_TRUE(ctx.called.load());
  EXPECT_TRUE(ctx.callback_thread == loop_thread);

  xDnsResultFree(ctx.result);
}

/* ───────────────────── IP literal passthrough ───────────────────── */

TEST_F(DnsTest, ResolveIPv4Literal) {
  DnsCtx ctx;

  xDnsQuery q = xDnsResolve(loop, "127.0.0.1", NULL, NULL, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());
  EXPECT_EQ(ctx.error, xErrno_Ok);
  EXPECT_GE(ctx.addr_count, 1);
  EXPECT_EQ(ctx.first_family, AF_INET);

  xDnsResultFree(ctx.result);
}

TEST_F(DnsTest, ResolveIPv6Literal) {
  DnsCtx ctx;

  xDnsQuery q = xDnsResolve(loop, "::1", NULL, NULL, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());
  EXPECT_EQ(ctx.error, xErrno_Ok);
  EXPECT_GE(ctx.addr_count, 1);

  xDnsResultFree(ctx.result);
}

/* ───────────────────── Service (port) resolution ───────────────────── */

TEST_F(DnsTest, ResolveWithService) {
  DnsCtx ctx;

  xDnsQuery q = xDnsResolve(loop, "localhost", "80", NULL, dns_callback, &ctx);
  ASSERT_NE(q, nullptr);

  RunUntilDone(ctx.called);

  EXPECT_TRUE(ctx.called.load());
  EXPECT_EQ(ctx.error, xErrno_Ok);
  EXPECT_GE(ctx.addr_count, 1);

  xDnsResultFree(ctx.result);
}
