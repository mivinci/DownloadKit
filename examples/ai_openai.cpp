/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ai_openai.cpp - Streaming REPL for an OpenAI-compatible endpoint
 *                 driven directly through the xagent provider layer.
 *
 * Why "provider-level" and not "agent"?
 *   The xagent module currently ships:
 *     - xagent/message, xagent/tool, xagent/provider (+ provider_openai)  — done
 *     - xagent/agent, xagent/session                                   — header-only
 *
 *   Once session.c / agent.c land, the canonical demo will use
 *   xAgentCreate + xAgentSessionCreate + xAgentSessionInput and never
 *   touch the provider vtable. For now, we drive the provider directly
 *   via the internal `ai_provider_submit` dispatcher so you can sanity-
 *   check the OpenAI provider end-to-end against a real endpoint.
 *
 *   Concretely that means this file includes provider_private.h, which
 *   is NOT a pattern user code should copy. It is explicitly marked
 *   internal in provider.h ("End users never call them directly").
 *
 * Usage:
 *   export LLM_API_URL="https://api.openai.com/v1"   # optional, no
 *                                                    # trailing slash
 *   export LLM_API_KEY="sk-xxx"
 *   export LLM_MODEL="gpt-4o"                        # optional
 *   ./ai_openai
 *
 * Features showcased:
 *   - Streaming text deltas rendered as they arrive.
 *   - Multi-turn history (each user line appends to the conversation,
 *     assistant replies get folded back in before the next turn).
 *   - One tool (`get_time`) advertised to the model. When the model
 *     asks to call it, we print the request. Because there is no
 *     session layer yet to run the tool-call loop, we do NOT feed the
 *     tool result back for another round; we just surface the intent.
 */

#include <xagent/message.h>
#include <xagent/provider.h>
#include <xagent/provider_openai.h>
#include <xagent/tool.h>
#include <xbase/event.h>
#include <xhttp/client.h>

/* Internal dispatcher — see file banner for justification. */
#include <xagent/provider_private.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/* ── A tiny "owning" conversation history ──────────────────────────────
 *
 * xAgentMessage / xAgentContent are borrow-only views. To hold a multi-turn
 * history across the REPL, we have to keep the backing strings alive
 * ourselves.
 */
struct OwnedTurn {
  xAgentRole                     role;
  std::string                 text;
  std::unique_ptr<xAgentContent> content; /* stable address; held by msg */
};

/* ── REPL state ─────────────────────────────────────────────────────── */

struct ReplCtx {
  xEventLoop  loop            = nullptr;
  bool        stream_done     = false; /* current submit() finished       */
  bool        saw_first_delta = false;
  std::string reply; /* accumulated assistant reply     */
};

/* ── Tool: get_time (demo only) ─────────────────────────────────────── */

static xErrno tool_get_time(xAgentQuery q, const xAgentContent *in, xAgentContent *out,
                            void *ud) {
  (void)q;
  (void)in;
  (void)ud;

  /* We only need to demonstrate dispatch here. The demo never actually
   * reaches this path because we do not feed tool results back into a
   * second provider round — that is the session layer's job. We still
   * implement the handler so the tool registration is complete. */
  static thread_local char buf[64];
  std::time_t              now = std::time(nullptr);
  std::tm                  tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &now);
#else
  gmtime_r(&now, &tm_utc);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

  out->type                     = xAgentContentType_ToolResult;
  out->u.tool_result.id         = "(unused)";
  out->u.tool_result.output     = buf;
  out->u.tool_result.output_len = std::strlen(buf);
  out->u.tool_result.is_error   = 0;
  return xErrno_Ok;
}

/* ── Provider streaming callbacks ───────────────────────────────────── */

static void on_text(const char *chunk, size_t len, void *arg) {
  auto *ctx = static_cast<ReplCtx *>(arg);
  if (!ctx->saw_first_delta) {
    std::putchar('\n');
    ctx->saw_first_delta = true;
  }
  std::fwrite(chunk, 1, len, stdout);
  std::fflush(stdout);
  ctx->reply.append(chunk, len);
}

static void on_tool_call(const xAgentContent *call, void *arg) {
  (void)arg;
  if (!call || call->type != xAgentContentType_ToolUse) return;
  std::printf("\n[tool_call] name=%s id=%s args=%s\n",
              call->u.tool_use.name ? call->u.tool_use.name : "(null)",
              call->u.tool_use.id ? call->u.tool_use.id : "(null)",
              call->u.tool_use.args_json ? call->u.tool_use.args_json : "{}");
  std::printf("[tool_call] note: demo does not execute the call — "
              "that requires the session layer (TODO).\n");
}

static const char *stop_reason_name(xAgentProviderStopReason r) {
  switch (r) {
  case xAgentProviderStop_EndTurn:
    return "end_turn";
  case xAgentProviderStop_ToolUse:
    return "tool_use";
  case xAgentProviderStop_MaxTokens:
    return "max_tokens";
  case xAgentProviderStop_StopSeq:
    return "stop_seq";
  case xAgentProviderStop_PromptLong:
    return "prompt_too_long";
  case xAgentProviderStop_Error:
    return "error";
  case xAgentProviderStop_Cancelled:
    return "cancelled";
  }
  return "?";
}

static void on_done(xAgentProviderStopReason reason, xErrno err,
                    const xAgentUsage *usage, const char *errmsg, void *arg) {
  (void)errmsg;
  auto *ctx = static_cast<ReplCtx *>(arg);

  /* Always surface the outcome so "silent failure" is impossible. */
  std::putchar('\n');
  std::printf("[done] reason=%s errno=%d reply_bytes=%zu",
              stop_reason_name(reason), (int)err, ctx->reply.size());
  if (usage) {
    /* -1 means "server was silent about this field" — show "?" so
     * the user can tell missing from zero. */
    auto fmt = [](int v, char *out, size_t n) {
      if (v < 0)
        std::snprintf(out, n, "?");
      else
        std::snprintf(out, n, "%d", v);
    };
    char p[16], c[16], t[16];
    fmt(usage->prompt_tokens, p, sizeof p);
    fmt(usage->completion_tokens, c, sizeof c);
    fmt(usage->total_tokens, t, sizeof t);
    std::printf(" tokens=%s/%s total=%s", p, c, t);
  }
  std::putchar('\n');
  std::fflush(stdout);

  ctx->stream_done = true;
  xEventLoopStop(ctx->loop);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main() {
  const char *api_url = std::getenv("LLM_API_URL");
  const char *api_key = std::getenv("LLM_API_KEY");
  const char *model   = std::getenv("LLM_MODEL");

  if (!api_key) {
    std::fprintf(stderr, "Please set at least LLM_API_KEY:\n"
                         "  export LLM_API_KEY=\"sk-xxx\"\n"
                         "  export LLM_API_URL=\"https://api.openai.com/v1\"  "
                         "(optional)\n"
                         "  export LLM_MODEL=\"gpt-4o\"                       "
                         "(optional)\n");
    return 1;
  }
  if (!model || model[0] == '\0') model = "gpt-4o";

  /* ── Event loop + HTTP client ───────────────────────────────────── */
  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    std::fprintf(stderr, "failed to create event loop\n");
    return 1;
  }
  xHttpClient http = xHttpClientCreate(loop, nullptr);
  if (!http) {
    std::fprintf(stderr, "failed to create http client\n");
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Provider ───────────────────────────────────────────────────── */
  xAgentOpenAIConf pconf;
  std::memset(&pconf, 0, sizeof(pconf));
  pconf.api_key       = api_key;
  pconf.base_url      = api_url; /* NULL = default */
  pconf.default_model = model;
  pconf.timeout_ms    = 60000;

  xAgentProvider pvd = xAgentProviderOpenAICreate(loop, http, &pconf);
  if (!pvd) {
    std::fprintf(stderr, "failed to create OpenAI provider\n");
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Tool: get_time ─────────────────────────────────────────────── */
  xAgentToolConf tconf;
  std::memset(&tconf, 0, sizeof(tconf));
  tconf.name        = "get_time";
  tconf.description = "Return the current UTC time in ISO-8601 format.";
  tconf.json_schema = "{\"type\":\"object\",\"properties\":{},"
                      "\"additionalProperties\":false}";
  tconf.handler     = tool_get_time;

  xAgentTool time_tool = xAgentToolCreate(&tconf);
  if (!time_tool) {
    std::fprintf(stderr, "failed to create tool\n");
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }
  const xAgentTool *tools[] = {&time_tool}; /* xAgentTool is opaque void*;
                                          * sconf.tools is xAgentTool**
                                          * (array of handle pointers) */

  ReplCtx ctx;
  ctx.loop = loop;
  xAgentProviderStreamCallbacks cbs;
  cbs.on_text      = on_text;
  cbs.on_tool_call = on_tool_call;
  cbs.on_done      = on_done;

  /* ── History ────────────────────────────────────────────────────── */
  std::vector<std::unique_ptr<OwnedTurn>> turns; /* stable addresses  */
  std::vector<xAgentMessage>                 history;

  /* Seed with a system message. */
  {
    auto t         = std::make_unique<OwnedTurn>();
    t->role        = xAgentRole_System;
    t->text        = "You are a concise assistant running on xKit's xagent "
                     "provider-level demo. Answer briefly.";
    t->content     = std::make_unique<xAgentContent>();
    *t->content    = xAgentContentText(t->text.c_str());
    xAgentMessage sys = xAgentMessageFromContent(xAgentRole_System, t->content.get(), 1);
    history.push_back(sys);
    turns.push_back(std::move(t));
  }

  std::printf("xagent provider-level REPL (model: %s)\n", model);
  std::printf("Type a message and press Enter. Ctrl-D or \"exit\" to quit.\n"
              "Tool 'get_time' is advertised (demo only — not executed).\n\n");

  char line[4096];
  while (true) {
    std::printf("> ");
    std::fflush(stdout);

    if (!std::fgets(line, sizeof(line), stdin)) break; /* EOF (Ctrl-D) */

    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0) continue;
    if (std::strcmp(line, "exit") == 0 || std::strcmp(line, "quit") == 0) break;

    /* Append user turn. */
    {
      auto t       = std::make_unique<OwnedTurn>();
      t->role      = xAgentRole_User;
      t->text      = line;
      t->content   = std::make_unique<xAgentContent>();
      *t->content  = xAgentContentText(t->text.c_str());
      xAgentMessage m = xAgentMessageFromContent(xAgentRole_User, t->content.get(), 1);
      history.push_back(m);
      turns.push_back(std::move(t));
    }

    /* ── Submit one round ─────────────────────────────────────────── */
    xAgentProviderSubmitConf sconf;
    std::memset(&sconf, 0, sizeof(sconf));
    sconf.model       = nullptr; /* provider falls back to default_model */
    sconf.messages    = history.data();
    sconf.n_messages  = history.size();
    sconf.tools       = tools;
    sconf.tools_count = sizeof(tools) / sizeof(tools[0]);
    sconf.temperature = -1.0; /* "not set" */

    ctx.stream_done     = false;
    ctx.saw_first_delta = false;
    ctx.reply.clear();

    xErrno err = ai_provider_submit(pvd, &sconf, &cbs, &ctx);
    if (err != xErrno_Ok) {
      std::fprintf(stderr, "[error] submit failed (errno=%d)\n", (int)err);
      /* Roll back the user turn we just appended. */
      history.pop_back();
      turns.pop_back();
      continue;
    }

    xEventLoopRun(loop);

    /* Append assistant reply to history if we got one. */
    if (!ctx.reply.empty()) {
      auto t      = std::make_unique<OwnedTurn>();
      t->role     = xAgentRole_Assistant;
      t->text     = std::move(ctx.reply);
      t->content  = std::make_unique<xAgentContent>();
      *t->content = xAgentContentText(t->text.c_str());
      xAgentMessage m =
        xAgentMessageFromContent(xAgentRole_Assistant, t->content.get(), 1);
      history.push_back(m);
      turns.push_back(std::move(t));
    }
  }

  std::printf("\nBye!\n");

  xAgentToolDestroy(time_tool);
  xAgentProviderDestroy(pvd);
  xHttpClientDestroy(http);
  xEventLoopDestroy(loop);
  return 0;
}
