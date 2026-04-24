/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ai_session.cpp - Streaming REPL driven by the full xai stack
 *                  (xAiAgent + xAiSession + xAiTool + xAiProvider).
 *
 * Unlike ai_openai.cpp (which drives the provider vtable directly for
 * end-to-end diagnostics), this demo is the canonical integration
 * path that user code should copy:
 *
 *   xAiProvider -> xAiAgent -> xAiSession
 *
 * The session hides the tool-call loop entirely: when the model asks
 * to call `get_time`, the session invokes our handler, folds the
 * result back into history, and submits another round on its own.
 * The REPL only sees streamed text plus a single on_done event per
 * user input.
 *
 * Usage:
 *   export LLM_API_URL="https://api.openai.com/v1"   # optional, no
 *                                                    # trailing slash
 *   export LLM_API_KEY="sk-xxx"
 *   export LLM_MODEL="gpt-4o"                        # optional
 *   ./ai_session
 */

#include <xai/agent.h>
#include <xai/message.h>
#include <xai/provider.h>
#include <xai/provider_openai.h>
#include <xai/session.h>
#include <xai/tool.h>
#include <xbase/event.h>
#include <xhttp/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

/* ── REPL state ─────────────────────────────────────────────────────── */

struct ReplCtx {
  xEventLoop loop            = nullptr;
  bool       saw_first_delta = false;
  bool       in_thinking     = false; /* currently streaming thinking? */
  size_t     reply_bytes     = 0;
};

/* ── Tool: get_time ─────────────────────────────────────────────────── */

static xErrno tool_get_time(const xAiContent *in, xAiContent *out, void *ud) {
  (void)in;
  (void)ud;

  /* Session strdup's our output before we return, so a thread-local
   * buffer is fine — we only need stability across the return. */
  static thread_local char buf[64];
  std::time_t              now = std::time(nullptr);
  std::tm                  tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &now);
#else
  gmtime_r(&now, &tm_utc);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

  out->type                     = xAiContentType_ToolResult;
  out->u.tool_result.id          = nullptr; /* session fills the id from
                                              * the matching tool_use */
  out->u.tool_result.output      = buf;
  out->u.tool_result.output_len  = std::strlen(buf);
  out->u.tool_result.is_error    = 0;
  return xErrno_Ok;
}

/* ── Session callbacks ──────────────────────────────────────────────── */

/* Close an open thinking block: reset SGR (`\x1b[0m`), newline, AND
 * emit one blank line so whatever follows (final text, [tool],
 * [done], ...) has visual breathing room. The trailing blank is
 * important when thinking ends with a sentence that looks like a
 * reply ("简短回复：..." etc) — without it the eye merges the faint
 * scratchpad into the bright answer. Must be called before printing
 * anything that shouldn't inherit faint style. No-op if no thinking
 * block is open, so it's safe to sprinkle liberally. */
static void end_thinking(ReplCtx *ctx) {
  if (!ctx->in_thinking) return;
  std::fputs("\x1b[0m\n\n", stdout);
  ctx->in_thinking = false;
}

static void on_text(xAiSession sess, const char *chunk, size_t len,
                    void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Close the thinking block (if any) before the visible reply
   * starts, so they don't visually merge AND the terminal doesn't
   * stay in faint mode for the rest of the output. */
  if (ctx->in_thinking) {
    end_thinking(ctx);
    ctx->saw_first_delta = true;
  } else if (!ctx->saw_first_delta) {
    std::putchar('\n');
    ctx->saw_first_delta = true;
  }
  std::fwrite(chunk, 1, len, stdout);
  std::fflush(stdout);
  ctx->reply_bytes += len;
}

/* Thinking stream: dim + prefix so it's obviously "model scratchpad"
 * and not the final answer. ANSI 2 = faint; most modern terminals
 * honour it (including macOS Terminal and iTerm2). On the rare
 * terminal that doesn't, the `[thinking]` prefix still telegraphs
 * intent. */
static void on_thinking(xAiSession sess, const char *chunk, size_t len,
                        void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  if (!ctx->in_thinking) {
    /* Open a new thinking block on its own line. */
    std::fputs("\n\x1b[2m[thinking] ", stdout);
    ctx->in_thinking = true;
  }
  std::fwrite(chunk, 1, len, stdout);
  std::fflush(stdout);
}

static void on_tool(xAiSession sess, const char *tool_name, int started,
                    void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* [tool] frames are chrome — render faint, same as
   * [thinking]/[done]. Node placement rules:
   *   - After thinking: end_thinking() already emitted `\n\n`, so
   *     just print the line (no leading newline from us).
   *   - Otherwise (start of run, or after text): print one blank
   *     line first so [tool] doesn't glue to whatever was above. */
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  if (!after_thinking) std::putchar('\n');
  std::printf("\x1b[2m[tool] %s %s\x1b[0m\n",
              tool_name ? tool_name : "(null)",
              started ? "starting" : "finished");
  std::fflush(stdout);
}

static const char *done_reason_name(xAiDoneReason r) {
  switch (r) {
    case xAiDoneReason_Completed:     return "completed";
    case xAiDoneReason_MaxTurns:      return "max_turns";
    case xAiDoneReason_PromptTooLong: return "prompt_too_long";
    case xAiDoneReason_Aborted:       return "aborted";
    case xAiDoneReason_ModelError:    return "model_error";
    case xAiDoneReason_ToolError:     return "tool_error";
    case xAiDoneReason_Stopped:       return "stopped";
  }
  return "?";
}

static void on_done(xAiSession sess, xAiDoneReason reason,
                    const xAiUsage *usage, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  /* [done] is chrome — render the whole line faint so it recedes and
   * the model's answer above stays visually primary. Extra blank
   * line after so the next `> ` prompt isn't glued to the status. */
  if (!after_thinking) std::putchar('\n');
  std::fputs("\x1b[2m", stdout);
  std::printf("[done] reason=%s reply_bytes=%zu",
              done_reason_name(reason), ctx->reply_bytes);
  /* Token accounting (cumulative across every round of this
   * xAiSessionInput). The provider fills -1 for fields it couldn't
   * parse; we hide those so the line stays clean for servers that
   * only report a subset. A NULL usage means the server never sent
   * a usage object — rare in practice (moonshot, openai, deepseek
   * all support stream_options.include_usage). */
  if (usage) {
    std::printf(" tokens=");
    if (usage->prompt_tokens >= 0) {
      std::printf("%d", usage->prompt_tokens);
    } else {
      std::printf("?");
    }
    std::printf("/");
    if (usage->completion_tokens >= 0) {
      std::printf("%d", usage->completion_tokens);
    } else {
      std::printf("?");
    }
    if (usage->total_tokens >= 0) {
      std::printf(" total=%d", usage->total_tokens);
    }
  }
  std::fputs("\x1b[0m\n\n", stdout);
  std::fflush(stdout);
  xEventLoopStop(ctx->loop);
}

static void on_error(xAiSession sess, xErrno err, const char *msg,
                     void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Same SGR hygiene as on_done — error might fire mid-thinking.
   * Errors are the one piece of chrome that should NOT recede — use
   * bold red (`\x1b[1;31m`) instead of faint so the user notices the
   * run failed at a glance. */
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  if (!after_thinking) std::fputc('\n', stderr);
  std::fprintf(stderr, "\x1b[1;31m[error] errno=%d msg=%s\x1b[0m\n\n",
               (int)err, msg ? msg : "(none)");
  std::fflush(stderr);
  xEventLoopStop(ctx->loop);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main() {
  const char *api_url = std::getenv("LLM_API_URL");
  const char *api_key = std::getenv("LLM_API_KEY");
  const char *model   = std::getenv("LLM_MODEL");

  if (!api_key) {
    std::fprintf(stderr,
                 "Please set at least LLM_API_KEY:\n"
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
  xAiOpenAIConf pconf;
  std::memset(&pconf, 0, sizeof(pconf));
  pconf.api_key       = api_key;
  pconf.base_url      = api_url;
  pconf.default_model = model;
  pconf.timeout_ms    = 60000;

  xAiProvider pvd = xAiProviderOpenAICreate(loop, http, &pconf);
  if (!pvd) {
    std::fprintf(stderr, "failed to create OpenAI provider\n");
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Tool: get_time ─────────────────────────────────────────────── */
  xAiToolConf tconf;
  std::memset(&tconf, 0, sizeof(tconf));
  tconf.name        = "get_time";
  tconf.description = "Return the current UTC time in ISO-8601 format.";
  tconf.json_schema = "{\"type\":\"object\",\"properties\":{},"
                      "\"additionalProperties\":false}";
  tconf.handler     = tool_get_time;

  xAiTool time_tool = xAiToolCreate(&tconf);
  if (!time_tool) {
    std::fprintf(stderr, "failed to create tool\n");
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }
  /* xAiTool is opaque void*; AgentConf.tools is `const xAiTool **`
   * (array of handle pointers), so store the handle's ADDRESS here,
   * not the handle itself. Same contract as provider's SubmitConf. */
  const xAiTool *tools[] = {&time_tool};

  /* ── Agent ──────────────────────────────────────────────────────── */
  xAiAgentConf aconf;
  std::memset(&aconf, 0, sizeof(aconf));
  aconf.loop          = loop;
  aconf.provider      = pvd;
  aconf.model         = model;
  aconf.system_prompt = "You are a concise assistant running on xKit's "
                        "xai session demo. Use the get_time tool when "
                        "the user asks about the current time. Keep "
                        "replies short.";
  aconf.tools         = tools;
  aconf.n_tools       = sizeof(tools) / sizeof(tools[0]);
  aconf.max_turns     = 8;

  xAiAgent agent = xAiAgentCreate(&aconf);
  if (!agent) {
    std::fprintf(stderr, "failed to create agent\n");
    xAiToolDestroy(time_tool);
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Session ────────────────────────────────────────────────────── */
  ReplCtx ctx;
  ctx.loop = loop;

  xAiSessionConf sconf;
  std::memset(&sconf, 0, sizeof(sconf));
  sconf.cbs.on_text     = on_text;
  sconf.cbs.on_thinking = on_thinking;
  sconf.cbs.on_tool     = on_tool;
  sconf.cbs.on_done     = on_done;
  sconf.cbs.on_error    = on_error;
  sconf.cbs.user_data   = &ctx;

  xAiSession sess = xAiSessionCreate(agent, &sconf);
  if (!sess) {
    std::fprintf(stderr, "failed to create session\n");
    xAiAgentDestroy(agent);
    xAiToolDestroy(time_tool);
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  std::printf("xai session REPL (model: %s)\n", model);
  std::printf("Type a message and press Enter. Ctrl-D or \"exit\" to quit.\n"
              "Tool 'get_time' is registered and will be executed when "
              "the model asks for it.\n\n");

  char line[4096];
  while (true) {
    std::printf("> ");
    std::fflush(stdout);

    if (!std::fgets(line, sizeof(line), stdin)) break; /* EOF */

    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0) continue;
    if (std::strcmp(line, "exit") == 0 || std::strcmp(line, "quit") == 0)
      break;

    ctx.saw_first_delta = false;
    ctx.in_thinking     = false;
    ctx.reply_bytes     = 0;

    /* xAiMessageFromText creates a User-role borrow-view that points
     * at `line` via a thread-local content slot (see message.c).
     * xAiSessionInput duplicates every byte into session-owned
     * memory before it returns, so reusing `line` on the next
     * iteration is safe. */
    xAiMessage m   = xAiMessageFromText(line);
    xErrno     err = xAiSessionInput(sess, m);
    if (err != xErrno_Ok) {
      std::fprintf(stderr, "[error] input rejected (errno=%d)\n",
                   (int)err);
      continue;
    }

    xEventLoopRun(loop);
  }

  std::printf("\nBye!\n");

  xAiSessionDestroy(sess);
  xAiAgentDestroy(agent);
  xAiToolDestroy(time_tool);
  xAiProviderDestroy(pvd);
  xHttpClientDestroy(http);
  xEventLoopDestroy(loop);
  return 0;
}
