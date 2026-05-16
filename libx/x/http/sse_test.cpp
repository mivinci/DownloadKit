/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sse_test.cpp - Unit tests for SSE (Server-Sent Events) support
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C" {
#include <x/http/client.h>
}

/* ───────────────────── Helpers ───────────────────── */

using ms = std::chrono::milliseconds;

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

/* ───────────────────── Minimal SSE server ───────────────────── */

/**
 * @brief A tiny SSE server that listens on a random port, accepts one
 *        connection, sends a canned SSE payload, then closes.
 *
 * Usage:
 *   MiniSseServer srv(payload);
 *   srv.start();
 *   // connect to srv.url() ...
 *   srv.join();
 */
class MiniSseServer {
public:
  explicit MiniSseServer(std::string payload, int delay_ms = 0)
      : payload_(std::move(payload)), delay_ms_(delay_ms) {}

  ~MiniSseServer() {
    join();
  }

  void start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd_, 0);

    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0; /* OS picks a free port */

    ASSERT_EQ(bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(listen_fd_, 1), 0);

    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, (struct sockaddr *)&addr, &len);
    port_ = ntohs(addr.sin_port);

    url_ = "http://127.0.0.1:" + std::to_string(port_) + "/events";

    thread_ = std::thread([this]() { serve(); });
  }

  void join() {
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
  }

  const std::string &url() const {
    return url_;
  }

private:
  void serve() {
    int client_fd = accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) return;

    /* Read the HTTP request (we don't care about contents) */
    char    buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf));
    (void)n;

    if (delay_ms_ > 0) std::this_thread::sleep_for(ms(delay_ms_));

    /* Send SSE response */
    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/event-stream\r\n"
                           "Cache-Control: no-cache\r\n"
                           "Connection: close\r\n"
                           "\r\n" +
                           payload_;

    ssize_t sent = write(client_fd, response.data(), response.size());
    (void)sent;

    /* Close to signal end of stream */
    close(client_fd);
  }

  std::string payload_;
  int         delay_ms_  = 0;
  int         listen_fd_ = -1;
  int         port_      = 0;
  std::string url_;
  std::thread thread_;
};

/* ───────────────────── Fixture ───────────────────── */

class SseClientTest : public ::testing::Test {
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

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(SseClientTest, NullClientReturnsError) {
  auto cb = [](const xSseEvent *, void *) -> int { return 0; };
  EXPECT_NE(
    xHttpClientGetSse(nullptr, "http://localhost/events", cb, nullptr, nullptr),
    xErrno_Ok);
}

TEST_F(SseClientTest, NullUrlReturnsError) {
  auto cb = [](const xSseEvent *, void *) -> int { return 0; };
  EXPECT_NE(xHttpClientGetSse(client, nullptr, cb, nullptr, nullptr),
            xErrno_Ok);
}

TEST_F(SseClientTest, NullOnEventReturnsError) {
  EXPECT_NE(xHttpClientGetSse(client, "http://localhost/events", nullptr,
                              nullptr, nullptr),
            xErrno_Ok);
}

/* ───────────────────── Basic SSE event reception ───────────────────── */

struct SseCtx {
  std::vector<std::string> events;
  std::vector<std::string> data;
  std::vector<std::string> ids;
  std::atomic<int>         event_count{0};
  std::atomic<bool>        done{false};
  int                      done_curl_code{-1};
};

static int on_sse_event(const xSseEvent *ev, void *arg) {
  auto *ctx = static_cast<SseCtx *>(arg);
  ctx->events.emplace_back(ev->event ? ev->event : "");
  ctx->data.emplace_back(ev->data ? ev->data : "");
  ctx->ids.emplace_back(ev->id ? ev->id : "");
  ctx->event_count.fetch_add(1, std::memory_order_release);
  return 0;
}

static void on_sse_done(int curl_code, void *arg) {
  auto *ctx           = static_cast<SseCtx *>(arg);
  ctx->done_curl_code = curl_code;
  ctx->done.store(true, std::memory_order_release);
}

TEST_F(SseClientTest, ReceiveSingleEvent) {
  std::string payload = "data: hello world\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE stream did not finish in time";
  EXPECT_EQ(ctx.event_count.load(), 1);
  EXPECT_EQ(ctx.events[0], "message"); /* default event type */
  EXPECT_EQ(ctx.data[0], "hello world");
}

TEST_F(SseClientTest, ReceiveMultipleEvents) {
  std::string payload = "data: first\n"
                        "\n"
                        "data: second\n"
                        "\n"
                        "data: third\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.event_count.load(), 3);
  EXPECT_EQ(ctx.data[0], "first");
  EXPECT_EQ(ctx.data[1], "second");
  EXPECT_EQ(ctx.data[2], "third");
}

TEST_F(SseClientTest, CustomEventType) {
  std::string payload = "event: custom_type\n"
                        "data: payload\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.event_count.load(), 1);
  EXPECT_EQ(ctx.events[0], "custom_type");
  EXPECT_EQ(ctx.data[0], "payload");
}

TEST_F(SseClientTest, MultilineData) {
  std::string payload = "data: line1\n"
                        "data: line2\n"
                        "data: line3\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.event_count.load(), 1);
  EXPECT_EQ(ctx.data[0], "line1\nline2\nline3");
}

TEST_F(SseClientTest, EventWithId) {
  std::string payload = "id: 42\n"
                        "data: with-id\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.event_count.load(), 1);
  EXPECT_EQ(ctx.ids[0], "42");
}

TEST_F(SseClientTest, CommentLinesIgnored) {
  std::string payload = ": this is a comment\n"
                        "data: real data\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  EXPECT_EQ(ctx.event_count.load(), 1);
  EXPECT_EQ(ctx.data[0], "real data");
}

/* ───────────────────── User-initiated close ───────────────────── */

static int on_sse_event_close_after_2(const xSseEvent *ev, void *arg) {
  auto *ctx = static_cast<SseCtx *>(arg);
  ctx->events.emplace_back(ev->event ? ev->event : "");
  ctx->data.emplace_back(ev->data ? ev->data : "");
  ctx->ids.emplace_back(ev->id ? ev->id : "");
  int count = ctx->event_count.fetch_add(1, std::memory_order_release) + 1;
  return (count >= 2) ? 1 : 0; /* close after 2nd event */
}

TEST_F(SseClientTest, UserCloseStopsStream) {
  std::string payload = "data: one\n\n"
                        "data: two\n\n"
                        "data: three\n\n"
                        "data: four\n\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(),
                                 on_sse_event_close_after_2, on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  /* Should have received at most 2 events (closed after 2nd) */
  EXPECT_EQ(ctx.event_count.load(), 2);
}

/* ───────────────────── on_done callback ───────────────────── */

TEST_F(SseClientTest, OnDoneCalledOnStreamEnd) {
  std::string payload = "data: hello\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  SseCtx ctx;
  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), on_sse_event,
                                 on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load());
  /* Server closes connection cleanly — curl_code should be CURLE_OK (0) */
  EXPECT_EQ(ctx.done_curl_code, 0);
}

TEST_F(SseClientTest, OnDoneNullDoesNotCrash) {
  std::string payload = "data: hello\n"
                        "\n";

  MiniSseServer srv(payload);
  srv.start();

  std::atomic<int> event_count{0};
  auto             cb = [](const xSseEvent *, void *arg) -> int {
    auto *c = static_cast<std::atomic<int> *>(arg);
    c->fetch_add(1, std::memory_order_release);
    return 0;
  };

  xErrno err = xHttpClientGetSse(client, srv.url().c_str(), cb,
                                 nullptr /* on_done = NULL */, &event_count);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until_count(loop, event_count, 1, 5000);

  /* Just verify it doesn't crash — pump a bit more for cleanup */
  for (int i = 0; i < 50; i++)
    xEventWait(loop, 10);

  EXPECT_GE(event_count.load(), 1);
}

/* ───────────────────── Destroy with in-flight SSE ───────────────────── */

TEST(SseLifecycle, DestroyWithInflightSse) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xHttpClient c = xHttpClientCreate(loop, nullptr);
  ASSERT_NE(c, nullptr);

  /* Server that delays before sending — simulates a long-lived stream */
  std::string   payload = "data: delayed\n"
                          "\n";
  MiniSseServer srv(payload, /*delay_ms=*/3000);
  srv.start();

  auto cb = [](const xSseEvent *, void *) -> int { return 0; };

  xErrno err = xHttpClientGetSse(c, srv.url().c_str(), cb, nullptr, nullptr);
  ASSERT_EQ(err, xErrno_Ok);

  /* Pump briefly to let curl start the connection */
  for (int i = 0; i < 10; i++)
    xEventWait(loop, 10);

  /* Destroy while SSE is in flight — must not crash or leak */
  xHttpClientDestroy(c);
  xEventLoopDestroy(loop);
}

/* ───────────────────── Connection failure ───────────────────── */

TEST_F(SseClientTest, ConnectionFailureCallsDone) {
  SseCtx ctx;

  /* Connect to a port that nobody is listening on */
  xErrno err = xHttpClientGetSse(client, "http://127.0.0.1:1/events",
                                 on_sse_event, on_sse_done, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  pump_until(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load())
    << "on_done was not called on connection failure";
  EXPECT_NE(ctx.done_curl_code, 0);     /* should be a curl error */
  EXPECT_EQ(ctx.event_count.load(), 0); /* no events received */
}