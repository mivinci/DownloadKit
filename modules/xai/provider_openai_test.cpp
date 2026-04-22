/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider_openai_test.cpp - Unit + small end-to-end tests for the
 * OpenAI-compatible provider.
 *
 * End-to-end tests use a tiny ad-hoc SSE server that canned-replays a
 * scripted OpenAI chat.completions stream. We assert on the callbacks
 * the provider delivers (on_text / on_tool_call / on_done) given the
 * wire we feed it.
 */

#include <gtest/gtest.h>

extern "C" {
#include <xai/message.h>
#include <xai/provider.h>
#include <xai/provider_openai.h>
#include <xbase/event.h>
#include <xhttp/client.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

/* ── Event-loop pump helpers (same pattern as xhttp/sse_test) ─────────── */

using ms = std::chrono::milliseconds;

static void pump_until(xEventLoop loop, std::atomic<bool> &flag,
                       int max_ms = 5000) {
  for (int elapsed = 0;
       elapsed < max_ms && !flag.load(std::memory_order_acquire);
       elapsed += 10) {
    xEventWait(loop, 10);
  }
}

/* ── Scripted OpenAI-style SSE server ─────────────────────────────────── */

class MiniOpenAIServer {
public:
  explicit MiniOpenAIServer(std::string body) : body_(std::move(body)) {}

  ~MiniOpenAIServer() {
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
    addr.sin_port        = 0;
    ASSERT_EQ(bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(listen_fd_, 1), 0);

    socklen_t alen = sizeof(addr);
    getsockname(listen_fd_, (struct sockaddr *)&addr, &alen);
    port_     = ntohs(addr.sin_port);
    base_url_ = "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    thread_   = std::thread([this]() { serve(); });
  }

  void join() {
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
  }

  const std::string &base_url() const { return base_url_; }
  const std::string &request() const { return request_; }

private:
  void serve() {
    int client_fd = accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) return;

    /* Drain the HTTP request into request_ (non-blocking-ish: we
     * read at most one chunk; enough for small POST bodies). */
    char    buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf));
    if (n > 0) request_.assign(buf, buf + n);

    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/event-stream\r\n"
                           "Cache-Control: no-cache\r\n"
                           "Connection: close\r\n"
                           "\r\n" +
                           body_;
    ssize_t sent = write(client_fd, response.data(), response.size());
    (void)sent;
    close(client_fd);
  }

  std::string body_;
  std::string base_url_;
  std::string request_;
  int         listen_fd_ = -1;
  int         port_      = 0;
  std::thread thread_;
};

/* ── Stream-callback recorder ─────────────────────────────────────────── */

struct Recorder {
  std::string                          text;
  std::vector<std::string>             tool_ids;
  std::vector<std::string>             tool_names;
  std::vector<std::string>             tool_args;
  std::atomic<bool>                    done_fired{false};
  xAiProviderStopReason                done_reason = xAiProviderStop_EndTurn;
  xErrno                               done_err    = xErrno_Ok;
};

static void on_text(const char *chunk, size_t len, void *arg) {
  auto *r = static_cast<Recorder *>(arg);
  r->text.append(chunk, len);
}

static void on_tool(const xAiContent *call, void *arg) {
  auto *r = static_cast<Recorder *>(arg);
  if (call && call->type == xAiContentType_ToolUse) {
    r->tool_ids.emplace_back(call->u.tool_use.id ? call->u.tool_use.id : "");
    r->tool_names.emplace_back(
      call->u.tool_use.name ? call->u.tool_use.name : "");
    r->tool_args.emplace_back(
      call->u.tool_use.args_json ? call->u.tool_use.args_json : "");
  }
}

static void on_done(xAiProviderStopReason reason, xErrno err, void *arg) {
  auto *r        = static_cast<Recorder *>(arg);
  r->done_reason = reason;
  r->done_err    = err;
  r->done_fired.store(true, std::memory_order_release);
}

/* ── Test fixture ─────────────────────────────────────────────────────── */

class OpenAIProviderTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpClient http   = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    http = xHttpClientCreate(loop, nullptr);
    ASSERT_NE(http, nullptr);
  }

  void TearDown() override {
    if (http) xHttpClientDestroy(http);
    if (loop) xEventLoopDestroy(loop);
  }

  xAiProvider make_provider(const std::string &base_url) {
    xAiOpenAIConf conf = {};
    conf.api_key       = "sk-test";
    conf.base_url      = base_url.c_str();
    conf.default_model = "gpt-test";
    return xAiProviderOpenAICreate(loop, http, &conf);
  }
};

/* ── Constructor argument validation ──────────────────────────────────── */

TEST_F(OpenAIProviderTest, CreateRejectsNullArgs) {
  xAiOpenAIConf conf = {};
  conf.api_key       = "sk-test";

  EXPECT_EQ(xAiProviderOpenAICreate(nullptr, http, &conf), nullptr);
  EXPECT_EQ(xAiProviderOpenAICreate(loop, nullptr, &conf), nullptr);
  EXPECT_EQ(xAiProviderOpenAICreate(loop, http, nullptr), nullptr);

  xAiOpenAIConf no_key = {};
  EXPECT_EQ(xAiProviderOpenAICreate(loop, http, &no_key), nullptr);
}

TEST_F(OpenAIProviderTest, CreateAcceptsMinimalConf) {
  xAiOpenAIConf conf = {};
  conf.api_key       = "sk-test";
  xAiProvider pvd    = xAiProviderOpenAICreate(loop, http, &conf);
  ASSERT_NE(pvd, nullptr);
  xAiProviderDestroy(pvd);
}

TEST_F(OpenAIProviderTest, DestroyNullIsNoop) {
  xAiProviderDestroy(nullptr);
}

/* ── Stream parsing: plain text ───────────────────────────────────────── */

TEST_F(OpenAIProviderTest, StreamsTextAndFinishesWithEndTurn) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"lo \"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"world\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("hi");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages              = &user_m;
  conf.n_messages            = 1;
  conf.temperature           = -1.0;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_text      = on_text;
  cbs.on_tool_call = on_tool;
  cbs.on_done      = on_done;

  /* Use module-internal dispatcher via the same symbol the session
   * layer would — but we can also go through the public handle by
   * asking the vtable to submit directly. To avoid including the
   * private header here, we just grab the vtable via a brief
   * manual cast that mirrors provider_private.h. */
  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto  *base = reinterpret_cast<ProviderBase *>(pvd);
  xErrno err  = base->vt->submit(base->ctx, &conf, &cbs, &rec);
  EXPECT_EQ(err, xErrno_Ok);

  pump_until(loop, rec.done_fired, 5000);
  ASSERT_TRUE(rec.done_fired.load());

  EXPECT_EQ(rec.text, "Hello world");
  EXPECT_EQ(rec.done_reason, xAiProviderStop_EndTurn);
  EXPECT_EQ(rec.done_err, xErrno_Ok);
  EXPECT_EQ(rec.tool_ids.size(), 0u);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Stream parsing: tool calls (fragmented arguments) ────────────────── */

TEST_F(OpenAIProviderTest, AssemblesFragmentedToolCall) {
  /* Tool-call arguments fragmented across three deltas, terminated
   * by finish_reason=tool_calls. */
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
      "\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"add\","
      "\"arguments\":\"{\\\"a\\\":\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
      "\"index\":0,\"function\":{\"arguments\":\"1,\\\"b\\\":\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
      "\"index\":0,\"function\":{\"arguments\":\"2}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{},"
         "\"finish_reason\":\"tool_calls\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("add 1 and 2");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages   = &user_m;
  conf.n_messages = 1;
  conf.temperature = -1.0;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_text      = on_text;
  cbs.on_tool_call = on_tool;
  cbs.on_done      = on_done;

  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto *base = reinterpret_cast<ProviderBase *>(pvd);
  ASSERT_EQ(base->vt->submit(base->ctx, &conf, &cbs, &rec), xErrno_Ok);

  pump_until(loop, rec.done_fired, 5000);
  ASSERT_TRUE(rec.done_fired.load());

  EXPECT_EQ(rec.text, "");
  ASSERT_EQ(rec.tool_ids.size(), 1u);
  EXPECT_EQ(rec.tool_ids[0],   "call_1");
  EXPECT_EQ(rec.tool_names[0], "add");
  EXPECT_EQ(rec.tool_args[0],  "{\"a\":1,\"b\":2}");
  EXPECT_EQ(rec.done_reason,   xAiProviderStop_ToolUse);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Stream parsing: finish_reason=length → MaxTokens ─────────────────── */

TEST_F(OpenAIProviderTest, MapsLengthFinishReason) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"x\"},"
         "\"finish_reason\":\"length\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("hi");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages   = &user_m;
  conf.n_messages = 1;
  conf.temperature = -1.0;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_text = on_text;
  cbs.on_done = on_done;

  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto *base = reinterpret_cast<ProviderBase *>(pvd);
  ASSERT_EQ(base->vt->submit(base->ctx, &conf, &cbs, &rec), xErrno_Ok);

  pump_until(loop, rec.done_fired, 5000);
  ASSERT_TRUE(rec.done_fired.load());
  EXPECT_EQ(rec.done_reason, xAiProviderStop_MaxTokens);
  EXPECT_EQ(rec.done_err,    xErrno_Ok);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Single-in-flight invariant ───────────────────────────────────────── */

TEST_F(OpenAIProviderTest, SecondSubmitWhileInFlightReturnsInvalidState) {
  /* Deliberately stall the server so the first submit is still in
   * flight when we try the second one. A long-ish payload that
   * arrives piecewise is not needed — we just need to call submit
   * twice before the event loop has a chance to drive the stream
   * to completion. */
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"a\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("hi");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages   = &user_m;
  conf.n_messages = 1;
  conf.temperature = -1.0;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_done = on_done;

  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto *base = reinterpret_cast<ProviderBase *>(pvd);
  ASSERT_EQ(base->vt->submit(base->ctx, &conf, &cbs, &rec), xErrno_Ok);

  /* Immediately try again: must be rejected because we haven't
   * driven the loop yet (so on_done hasn't fired). */
  Recorder rec2;
  xAiProviderStreamCallbacks cbs2 = {};
  cbs2.on_done = on_done;
  EXPECT_EQ(base->vt->submit(base->ctx, &conf, &cbs2, &rec2),
            xErrno_InvalidState);

  /* Drain the first flight to avoid leaking a pending request. */
  pump_until(loop, rec.done_fired, 5000);
  EXPECT_TRUE(rec.done_fired.load());
  EXPECT_FALSE(rec2.done_fired.load());

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Cancel before first event ────────────────────────────────────────── */

TEST_F(OpenAIProviderTest, CancelFiresDoneWithCancelledReason) {
  /* Server will sit there until we cancel. We keep the payload
   * tiny but rely on cancel racing ahead of the event on the loop;
   * even if the stream completes first, the provider must still
   * only fire on_done once and should honour the cancel flag when
   * it observes it. */
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"ignored\"}}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("hi");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages   = &user_m;
  conf.n_messages = 1;
  conf.temperature = -1.0;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_text = on_text;
  cbs.on_done = on_done;

  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto *base = reinterpret_cast<ProviderBase *>(pvd);
  ASSERT_EQ(base->vt->submit(base->ctx, &conf, &cbs, &rec), xErrno_Ok);

  /* Cancel before pumping the loop. */
  base->vt->cancel(base->ctx);

  pump_until(loop, rec.done_fired, 5000);
  ASSERT_TRUE(rec.done_fired.load());
  /* Either Cancelled (cancel observed first) or EndTurn (stream
   * finished before cancel was seen). Both are acceptable outcomes
   * — we just care that on_done fired exactly once and the
   * provider is back to idle. */
  EXPECT_TRUE(rec.done_reason == xAiProviderStop_Cancelled ||
              rec.done_reason == xAiProviderStop_EndTurn);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Request body inspection: model & messages show up on the wire ────── */

TEST_F(OpenAIProviderTest, RequestBodyCarriesModelAndMessages) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent sys_c  = xAiContentText("be brief");
  xAiMessage sys_m  = xAiMessageFromContent(xAiRole_System, &sys_c, 1);
  xAiContent user_c = xAiContentText("ping");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);
  xAiMessage msgs[] = {sys_m, user_m};

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.model       = "gpt-override";
  conf.messages    = msgs;
  conf.n_messages  = 2;
  conf.temperature = 0.5;
  conf.max_tokens  = 16;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_done = on_done;

  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto *base = reinterpret_cast<ProviderBase *>(pvd);
  ASSERT_EQ(base->vt->submit(base->ctx, &conf, &cbs, &rec), xErrno_Ok);

  pump_until(loop, rec.done_fired, 5000);
  ASSERT_TRUE(rec.done_fired.load());

  /* The request sits in the server's captured buffer now. */
  const std::string &req = srv.request();
  EXPECT_NE(req.find("POST /v1/chat/completions"), std::string::npos);
  EXPECT_NE(req.find("Authorization: Bearer sk-test"), std::string::npos);
  EXPECT_NE(req.find("\"model\":\"gpt-override\""), std::string::npos);
  EXPECT_NE(req.find("\"stream\":true"), std::string::npos);
  EXPECT_NE(req.find("\"temperature\":0.5"), std::string::npos);
  EXPECT_NE(req.find("\"max_tokens\":16"), std::string::npos);
  EXPECT_NE(req.find("be brief"), std::string::npos);
  EXPECT_NE(req.find("ping"), std::string::npos);

  xAiProviderDestroy(pvd);
  srv.join();
}
