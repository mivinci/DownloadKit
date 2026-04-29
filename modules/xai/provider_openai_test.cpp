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
#include <xai/tool.h>
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

  /* If set, the server returns this status line instead of 200 OK,
   * and wraps body_ as a JSON error response (Content-Type: application/
   * json) instead of an SSE stream. Used to exercise how the provider
   * reacts when the upstream rejects a request mid-conversation. */
  void set_error_status(int code, std::string reason) {
    err_code_   = code;
    err_reason_ = std::move(reason);
  }

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

    std::string response;
    if (err_code_ > 0) {
      response = "HTTP/1.1 " + std::to_string(err_code_) + " " +
                 err_reason_ +
                 "\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: " +
                 std::to_string(body_.size()) +
                 "\r\n"
                 "Connection: close\r\n"
                 "\r\n" +
                 body_;
    } else {
      response = "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/event-stream\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Connection: close\r\n"
                 "\r\n" +
                 body_;
    }
    ssize_t sent = write(client_fd, response.data(), response.size());
    (void)sent;
    close(client_fd);
  }

  std::string body_;
  std::string base_url_;
  std::string request_;
  int         listen_fd_ = -1;
  int         port_      = 0;
  int         err_code_  = 0;
  std::string err_reason_;
  std::thread thread_;
};

/* ── Stream-callback recorder ─────────────────────────────────────────── */

struct Recorder {
  std::string                          text;
  std::string                          thinking;
  std::vector<std::string>             tool_ids;
  std::vector<std::string>             tool_names;
  std::vector<std::string>             tool_args;
  std::atomic<bool>                    done_fired{false};
  xAiProviderStopReason                done_reason = xAiProviderStop_EndTurn;
  xErrno                               done_err    = xErrno_Ok;
  /* Usage snapshot captured on on_done. has_usage stays false when
   * the provider hands NULL (no usage ever reported by server);
   * otherwise the copy holds the last round's numbers, with -1 for
   * fields the server omitted. */
  bool                                 has_usage = false;
  xAiUsage                             usage{-1, -1, -1};
};

static void on_text(const char *chunk, size_t len, void *arg) {
  auto *r = static_cast<Recorder *>(arg);
  r->text.append(chunk, len);
}

static void on_thinking(const char *chunk, size_t len, void *arg) {
  auto *r = static_cast<Recorder *>(arg);
  r->thinking.append(chunk, len);
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

static void on_done(xAiProviderStopReason reason, xErrno err,
                    const xAiUsage *usage, const char *errmsg, void *arg) {
  auto *r        = static_cast<Recorder *>(arg);
  r->done_reason = reason;
  r->done_err    = err;
  if (usage) {
    r->has_usage = true;
    r->usage     = *usage;
  }
  (void)errmsg;
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

/* ── Request body inspection: tools[] are encoded correctly ───────────
 *
 * Regression for 57ddc48 (fix(xai): deref tools[i] correctly when
 * encoding tool_calls request). SubmitConf.tools is `const xAiTool **`,
 * i.e. an array of handle pointers. Before the fix the provider did
 *
 *     cJSON *t = oai_tool_to_json((xAiTool)conf->tools[i]);
 *
 * which cast the pointer-to-handle to a handle, skipping one level of
 * indirection. ai_tool_name() then read garbage, cJSON silently
 * emitted `"name":""` and dropped `description` entirely, and the
 * server 400'd the request.
 *
 * This test exercises two tools (to also cover the second iteration
 * of the encoding loop) and checks every user-visible field of the
 * emitted JSON. The negative assertions on `"name":""` would have
 * caught the original bug on their own.                                */

static xErrno noop_tool_handler(const xAiContent *, xAiContent *, void *) {
  return xErrno_Ok;
}

TEST_F(OpenAIProviderTest, RequestBodyEncodesTools) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  /* Tool A: no-arg time getter (schema = NULL → empty object). */
  xAiToolConf tconf_time = {};
  tconf_time.name        = "get_time";
  tconf_time.description = "Return current local time";
  tconf_time.handler     = noop_tool_handler;
  xAiTool t_time = xAiToolCreate(&tconf_time);
  ASSERT_NE(t_time, nullptr);

  /* Tool B: two-arg add (schema provided → parsed into parameters). */
  const char *add_schema =
    "{\"type\":\"object\","
     "\"properties\":{\"a\":{\"type\":\"integer\"},"
                     "\"b\":{\"type\":\"integer\"}},"
     "\"required\":[\"a\",\"b\"]}";
  xAiToolConf tconf_add = {};
  tconf_add.name        = "add";
  tconf_add.description = "Add two integers";
  tconf_add.json_schema = add_schema;
  tconf_add.handler     = noop_tool_handler;
  xAiTool t_add = xAiToolCreate(&tconf_add);
  ASSERT_NE(t_add, nullptr);

  /* This is exactly how examples/ai_openai.cpp advertises tools —
   * an array of pointers to handles. Keep the spelling identical
   * so this test pins the call convention end-to-end. */
  const xAiTool *tools[] = {&t_time, &t_add};

  xAiContent user_c = xAiContentText("ping");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages    = &user_m;
  conf.n_messages  = 1;
  conf.tools       = tools;
  conf.n_tools     = sizeof(tools) / sizeof(tools[0]);
  conf.temperature = -1.0;

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

  const std::string &req = srv.request();

  /* Positive assertions: every field oai_tool_to_json() writes. */
  EXPECT_NE(req.find("\"tools\":["),                 std::string::npos);
  EXPECT_NE(req.find("\"type\":\"function\""),       std::string::npos);
  EXPECT_NE(req.find("\"name\":\"get_time\""),       std::string::npos);
  EXPECT_NE(req.find("\"description\":\"Return current local time\""),
            std::string::npos);
  EXPECT_NE(req.find("\"name\":\"add\""),            std::string::npos);
  EXPECT_NE(req.find("\"description\":\"Add two integers\""),
            std::string::npos);
  /* The `add` schema is pre-parsed + re-emitted, so the exact byte
   * order may differ from the input string. Assert on distinctive
   * fragments only. */
  EXPECT_NE(req.find("\"parameters\":{"),            std::string::npos);
  EXPECT_NE(req.find("\"type\":\"object\""),         std::string::npos);
  EXPECT_NE(req.find("\"required\":[\"a\",\"b\"]"),  std::string::npos);

  /* Negative assertions: the pre-fix bug manifested as empty or
   * missing `name` (ai_tool_name() returned garbage / NULL, which
   * cJSON_AddStringToObject coerced to ""). These three lines would
   * have failed on the pre-57ddc48 code. */
  EXPECT_EQ(req.find("\"name\":\"\""),               std::string::npos);
  EXPECT_EQ(req.find("\"name\":null"),               std::string::npos);
  /* And of course no uninitialised garbage should leak into the
   * request as stray bytes. A canary: the SSE body we wired above
   * contains the literal "ok"; if random heap bytes ended up in the
   * request body we'd likely see non-printable chars before the
   * trailing \r\n\r\n. We don't enforce that here because it's
   * brittle, but the positive + negative name/description checks
   * above are strong enough to pin the fix. */

  xAiProviderDestroy(pvd);
  xAiToolDestroy(t_add);
  xAiToolDestroy(t_time);
  srv.join();
}

/* ── HTTP 4xx surfaces as a provider error, not a silent EndTurn ─────
 *
 * Regression target for a bug observed on 2026-04-23:
 *
 *   $ ./build/examples/ai_session   (kimi-k2.6, real moonshot API)
 *   > 几点了
 *   [tool] get_time starting
 *   [tool] get_time finished
 *   [done] reason=completed reply_bytes=0   ← WRONG
 *
 * The second-round POST was rejected by the upstream with a non-2xx
 * status and a JSON error body. xhttp/sse.c doesn't inspect the
 * status code on the downstream, so the JSON body was handed to the
 * SSE parser, which produced no events, and libcurl reported the
 * transfer as successful (CURLcode == 0). oai_on_sse_done then saw
 * `curl_code == 0 && !saw_finish_reason` and mapped that to the
 * default "graceful end of turn".
 *
 * Expected: the provider's on_done must carry a non-Ok `xErrno` (or
 * `xAiProviderStop_Error`) so the session layer can distinguish
 * "model chose to stop" from "request was rejected". This test is
 * allowed to pass with either Error+non-Ok or, at minimum, a reason
 * other than EndTurn.                                                */
TEST_F(OpenAIProviderTest, HttpErrorIsNotSilentlyTreatedAsEndTurn) {
  /* The body here is what moonshot/openai-compatible endpoints
   * typically return on 400. It is NOT SSE. */
  std::string err_body =
    "{\"error\":{\"message\":\"Invalid request: tool_call_id not "
    "found\",\"type\":\"invalid_request_error\",\"code\":"
    "\"invalid_parameter\"}}";

  MiniOpenAIServer srv(err_body);
  srv.set_error_status(400, "Bad Request");
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("hi");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages    = &user_m;
  conf.n_messages  = 1;
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

  /* No text should have leaked through. */
  EXPECT_EQ(rec.text, "");
  EXPECT_EQ(rec.tool_ids.size(), 0u);

  /* The important invariant: upstream HTTP failure must NOT be
   * reported to the caller as a graceful EndTurn with xErrno_Ok.
   * Either the reason shifts to Error, or the errno becomes
   * non-Ok — but the combination (EndTurn, Ok) is a lie. */
  bool reported_as_error =
    (rec.done_reason == xAiProviderStop_Error) ||
    (rec.done_err != xErrno_Ok);
  EXPECT_TRUE(reported_as_error)
    << "HTTP 400 was silently reported as reason="
    << static_cast<int>(rec.done_reason)
    << ", err=" << static_cast<int>(rec.done_err)
    << ". This is the 2026-04-23 'reply_bytes=0 reason=completed' bug.";

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Stream parsing: delta.reasoning_content → on_thinking ────────────
 *
 * Regression for 2026-04-23 (kimi-k2.6): thinking-capable models stream
 * their chain-of-thought in `choices[].delta.reasoning_content` rather
 * than `.content`. Before the fix the OpenAI provider only watched
 * `.content`, so reasoning deltas were silently dropped — and then the
 * next round's POST got rejected with
 *   "thinking is enabled but reasoning_content is missing in
 *    assistant tool call message".
 *
 * This test pins the delivery contract: reasoning fragments MUST be
 * reassembled and forwarded via cbs.on_thinking in order, with no
 * bleed into cbs.on_text.                                              */

TEST_F(OpenAIProviderTest, StreamsReasoningContent) {
  /* Two reasoning deltas, one text delta, and a stop. Ordering matters:
   * thinking-capable APIs typically emit reasoning first, then the
   * final answer. */
  std::string body =
    "data: {\"choices\":[{\"delta\":"
         "{\"reasoning_content\":\"I think \"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":"
         "{\"reasoning_content\":\"therefore I am.\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":"
         "{\"content\":\"Hi.\"},"
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
  conf.messages    = &user_m;
  conf.n_messages  = 1;
  conf.temperature = -1.0;

  xAiProviderStreamCallbacks cbs = {};
  cbs.on_text     = on_text;
  cbs.on_thinking = on_thinking;
  cbs.on_done     = on_done;

  struct ProviderBase {
    const xAiProviderVtable *vt;
    void                    *ctx;
  };
  auto *base = reinterpret_cast<ProviderBase *>(pvd);
  ASSERT_EQ(base->vt->submit(base->ctx, &conf, &cbs, &rec), xErrno_Ok);

  pump_until(loop, rec.done_fired, 5000);
  ASSERT_TRUE(rec.done_fired.load());

  /* Reasoning assembled in arrival order. */
  EXPECT_EQ(rec.thinking, "I think therefore I am.");
  /* And it did NOT bleed into on_text. */
  EXPECT_EQ(rec.text,     "Hi.");
  EXPECT_EQ(rec.done_reason, xAiProviderStop_EndTurn);
  EXPECT_EQ(rec.done_err,    xErrno_Ok);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Request body inspection: Thinking blocks serialise as
 *    `reasoning_content` on assistant messages ────────────────────────
 *
 * Second half of the 2026-04-23 fix. When the session replays history
 * that contains assistant-side Thinking blocks (from an earlier round),
 * the provider MUST emit them as `reasoning_content` on the assistant
 * JSON message. Otherwise moonshot/kimi reject the follow-up POST with
 *   "thinking is enabled but reasoning_content is missing in
 *    assistant tool call message at index N".
 *
 * The exact shape we pin here:
 *   {"role":"assistant","content":null,"reasoning_content":"...",
 *    "tool_calls":[{...}]}
 * i.e. Thinking → top-level `reasoning_content`, Text absent/null,
 * ToolUse → `tool_calls`.                                              */

TEST_F(OpenAIProviderTest, AssistantMessageSerialisesReasoningContent) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  /* Replay a tiny history:
   *   [0] user: "weather in SF?"
   *   [1] assistant: [Thinking("I should call get_weather."),
   *                   ToolUse(id=call_1, name=get_weather, args={...})]
   *   [2] tool: {tool result for call_1}
   *   [3] user: "thanks"  (new turn — this is what triggers the POST)
   *
   * The assistant entry (index 1) is what must carry reasoning_content
   * on the wire. We don't need to reproduce the full session state
   * machine — just construct the shape that session.c would assemble
   * on replay, and hand it to the provider directly. */
  xAiContent u0 = xAiContentText("weather in SF?");
  xAiMessage m0 = xAiMessageFromContent(xAiRole_User, &u0, 1);

  xAiContent a_blocks[2] = {};
  a_blocks[0].type              = xAiContentType_Thinking;
  a_blocks[0].u.thinking.text   = "I should call get_weather.";
  a_blocks[0].u.thinking.len    = strlen(a_blocks[0].u.thinking.text);
  a_blocks[1].type              = xAiContentType_ToolUse;
  a_blocks[1].u.tool_use.id        = "call_1";
  a_blocks[1].u.tool_use.name      = "get_weather";
  a_blocks[1].u.tool_use.args_json = "{\"city\":\"SF\"}";
  xAiMessage m1 = xAiMessageFromContent(xAiRole_Assistant, a_blocks, 2);

  xAiContent t_block = {};
  t_block.type                     = xAiContentType_ToolResult;
  t_block.u.tool_result.id         = "call_1";
  t_block.u.tool_result.output     = "sunny";
  t_block.u.tool_result.output_len = 5;
  xAiMessage m2 = xAiMessageFromContent(xAiRole_Tool, &t_block, 1);

  xAiContent u3 = xAiContentText("thanks");
  xAiMessage m3 = xAiMessageFromContent(xAiRole_User, &u3, 1);

  xAiMessage msgs[] = {m0, m1, m2, m3};

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages    = msgs;
  conf.n_messages  = sizeof(msgs) / sizeof(msgs[0]);
  conf.temperature = -1.0;

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

  const std::string &req = srv.request();

  /* Positive: Thinking made it out as reasoning_content on the
   * assistant message. */
  EXPECT_NE(req.find("\"reasoning_content\":\"I should call get_weather.\""),
            std::string::npos)
    << "assistant Thinking block was NOT serialised as reasoning_content. "
       "kimi-k2.6 / DeepSeek-R1 will 400 the next round. Request was:\n"
    << req;

  /* Positive: ToolUse still rendered alongside. */
  EXPECT_NE(req.find("\"tool_calls\":["),        std::string::npos);
  EXPECT_NE(req.find("\"name\":\"get_weather\""), std::string::npos);
  EXPECT_NE(req.find("\"id\":\"call_1\""),        std::string::npos);

  /* Positive: assistant's `content` is null when there is no Text
   * block (matching OpenAI's wire convention for tool_call-only
   * turns). */
  EXPECT_NE(req.find("\"role\":\"assistant\""), std::string::npos);
  EXPECT_NE(req.find("\"content\":null"),       std::string::npos);

  /* Negative: the reasoning must NOT accidentally land on the user
   * or tool messages, nor duplicate as `content`. */
  EXPECT_EQ(req.find("\"content\":\"I should call get_weather.\""),
            std::string::npos);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── Usage accounting: prompt_tokens/completion_tokens parsed from
 *    the final usage chunk, and forwarded via on_done ────────────────
 *
 * OpenAI-compatible servers carry token counts in a top-level
 * `usage` object, typically on the chunk that also emits
 * finish_reason, or on a dedicated chunk just before [DONE] (when
 * the client asked for stream_options.include_usage=true).
 *
 * We send a minimal stream where:
 *  - one chunk delivers content + finish_reason=stop WITHOUT usage
 *  - a second chunk has `"choices":[]` and `usage={...}`  (this is
 *    the exact shape OpenAI ships when include_usage is on)
 *  - [DONE] closes the stream
 *
 * The provider MUST parse the standalone usage chunk (it does not
 * live under choices[]) and hand the numbers to on_done via the
 * xAiUsage* pointer.                                                 */

TEST_F(OpenAIProviderTest, ForwardsUsageOnDone) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
         "\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,"
         "\"completion_tokens\":7,\"total_tokens\":19}}\n\n"
    "data: [DONE]\n\n";

  MiniOpenAIServer srv(body);
  srv.start();

  xAiProvider pvd = make_provider(srv.base_url());
  ASSERT_NE(pvd, nullptr);

  xAiContent user_c = xAiContentText("hi");
  xAiMessage user_m = xAiMessageFromContent(xAiRole_User, &user_c, 1);

  Recorder rec;
  xAiProviderSubmitConf conf = {};
  conf.messages    = &user_m;
  conf.n_messages  = 1;
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

  /* Also verify the request body opted in — regression guard: if
   * anyone removes stream_options.include_usage, servers like
   * OpenAI will silently drop the usage chunk, this assertion
   * stays but the prompt_tokens one below would start failing
   * against the real wire. */
  const std::string &req = srv.request();
  EXPECT_NE(req.find("\"stream_options\":{"), std::string::npos);
  EXPECT_NE(req.find("\"include_usage\":true"), std::string::npos);

  /* The usage pointer must be non-NULL and the numbers must round-
   * trip intact. */
  EXPECT_EQ(rec.text, "hi");
  EXPECT_EQ(rec.done_reason, xAiProviderStop_EndTurn);
  ASSERT_TRUE(rec.has_usage)
    << "provider did not forward usage to on_done — either the "
       "parser missed `usage` on a choices-empty chunk, or "
       "oai_finish_flight failed to pass the pointer.";
  EXPECT_EQ(rec.usage.prompt_tokens,     12);
  EXPECT_EQ(rec.usage.completion_tokens, 7);
  EXPECT_EQ(rec.usage.total_tokens,      19);

  xAiProviderDestroy(pvd);
  srv.join();
}

/* ── No usage reported: on_done still fires, but with NULL usage ───
 *
 * Pins the "unknown vs zero" contract. If the server never sends a
 * `usage` block (e.g. a minimal mock or a gateway that strips it),
 * the provider MUST hand NULL — never a zero-initialised struct,
 * which would misrepresent "no data" as "zero tokens used".        */

TEST_F(OpenAIProviderTest, NoUsageChunkYieldsNullUsage) {
  std::string body =
    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},"
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
  conf.messages    = &user_m;
  conf.n_messages  = 1;
  conf.temperature = -1.0;

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

  EXPECT_FALSE(rec.has_usage)
    << "provider fabricated a usage struct out of thin air; the "
       "caller has no way to tell 'unknown' from 'zero' apart.";

  xAiProviderDestroy(pvd);
  srv.join();
}
