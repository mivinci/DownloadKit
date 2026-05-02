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
 *   ./ai_session [-d <path>]                         # default: cwd
 *
 * The REPL uses xline for CJK-aware line editing, persistent
 * history (stored at <data_dir>/.ai_session_history), and Ctrl-R
 * reverse search. See cmake/FindIsocline.cmake.
 */

#include <xai/agent.h>
#include <xai/message.h>
#include <xai/provider.h>
#include <xai/provider_openai.h>
#include <xai/session.h>
#include <xai/tool.h>
#include <xai/tool_shell.h>
#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xbase/time.h>
#include <xhttp/client.h>

#include <line.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

/* ── REPL state ─────────────────────────────────────────────────────── */
struct ReplCtx {
  xEventLoop loop             = nullptr;
  xAiSession  sess            = nullptr;
  bool       saw_first_delta  = false;
  bool       in_thinking      = false; /* currently streaming thinking? */
  size_t     reply_bytes      = 0;
  int        total_tokens     = 0;   /* cumulative across all rounds */
  size_t     budget_limit     = 0;   /* from last GatePassed event */
  size_t     budget_remaining = 0;   /* from last GatePassed event */
  double     budget_factor    = 1.0; /* EWMA calibrator factor */
  size_t     budget_samples   = 0;   /* calibrator observation count */
  size_t     budget_estimated = 0;   /* calibrated pre-submit estimate */
  int last_actual_prompt = -1; /* provider-reported first-round prompt_tokens */
  uint64_t input_ms      = 0;  /* monotonic timestamp (ms) at user input */
  bool     should_exit   = false; /* set by /exit handler */
  const char *hist_path  = nullptr; /* xline history file, for /history */
};

/* ── Slash command table ───────────────────────────────────────────────
 *
 * Commands typed at the prompt that start with '/' are intercepted by
 * the REPL and never sent to the model. Keeping them in a static table
 * gives us three things for free:
 *
 *   1) a completer data source (xline scans the table on Tab),
 *   2) a one-liner help text used both by `/help` and by the inline
 *      hint xline renders in faint text when the prefix resolves
 *      to a single match (fish-shell style),
 *   3) a single place to register new commands — handler + name +
 *      help live together so nothing drifts.
 *
 * Handlers receive the ReplCtx plus any argument tail after the first
 * space (trimmed of leading whitespace). They may be NULL for commands
 * whose entire effect is a side-channel flag flip handled in the main
 * loop (currently none; `/exit` sets ctx->should_exit).
 *
 * The handler for a specific command is looked up by exact match on
 * the first whitespace-delimited token, so `/tokens` doesn't collide
 * with a hypothetical future `/tokenize`. */
typedef void (*SlashCmdFunc)(ReplCtx *ctx, const char *args);

struct SlashCmd {
  const char   *name; /* including leading '/' */
  const char   *help;
  SlashCmdFunc  fn;
};

static void slash_cmd_help(ReplCtx *ctx, const char *args);
static void slash_cmd_exit(ReplCtx *ctx, const char *args);
static void slash_cmd_clear(ReplCtx *ctx, const char *args);
static void slash_cmd_history(ReplCtx *ctx, const char *args);
static void slash_cmd_tokens(ReplCtx *ctx, const char *args);

static const SlashCmd g_slash_cmds[] = {
  {"/help",    "show this help",                slash_cmd_help},
  {"/exit",    "quit the REPL",                 slash_cmd_exit},
  {"/clear",   "clear the terminal screen",     slash_cmd_clear},
  {"/history", "print input history",           slash_cmd_history},
  {"/tokens",  "show cumulative token usage",   slash_cmd_tokens},
};
static const size_t g_slash_cmds_count =
  sizeof(g_slash_cmds) / sizeof(g_slash_cmds[0]);

/* Character-class predicate for xLineCompleteWord: returns true when
 * `c` should be considered part of the current completion token.
 * Slash commands are ASCII letters/digits/underscore plus the leading
 * '/', so we accept all of those. Everything else (space, punctuation)
 * is a token boundary, which makes Tab inside the args of a command
 * (e.g. `/tokens <cursor>`) stay quiet instead of trying to re-match
 * the command name. */
static bool is_slash_cmd_char(const char *s, long len) {
  if (len != 1) return false;  /* ASCII only; no multi-byte in cmd names */
  char c = s[0];
  return c == '/' || c == '_' || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Inner completer invoked by xLineCompleteWord with the already-
 * extracted token. Because we went through xLineCompleteWord, every
 * xLineAddCompletion* call here is treated as a *replacement* for
 * `prefix` — xline handles the delete-before bookkeeping, so we
 * just hand it the full command name. */
static void slash_completer_inner(xLineCompletionEnv *cenv,
                                  const char *prefix) {
  if (!prefix || prefix[0] != '/') return;
  for (size_t i = 0; i < g_slash_cmds_count; ++i) {
    const SlashCmd *c = &g_slash_cmds[i];
    if (xLineStartsWith(c->name, prefix)) {
      xLineAddCompletionEx(cenv, c->name, c->name, c->help);
    }
  }
}

/* Top-level completer registered with xline. We delegate to
 * xLineCompleteWord so prefix extraction and replacement semantics
 * are consistent with the rest of the library — otherwise Tab on
 * `/cl` would append `/clear` producing `/cl/clear` (and the inline
 * hint renders the same broken overlay). */
static void slash_completer(xLineCompletionEnv *cenv, const char *prefix) {
  (void)prefix;  /* xLineCompleteWord re-derives it using our predicate */
  xLineCompleteWord(cenv, prefix, slash_completer_inner, is_slash_cmd_char);
}

static void slash_cmd_help(ReplCtx *ctx, const char *args) {
  (void)ctx;
  (void)args;
  std::puts("Available commands:");
  for (size_t i = 0; i < g_slash_cmds_count; ++i) {
    std::printf("  \x1b[1m%-10s\x1b[0m %s\n", g_slash_cmds[i].name,
                g_slash_cmds[i].help);
  }
  std::puts("Anything not starting with '/' is sent to the model.");
  std::puts("Tip: type '/' and press Tab to browse commands.");
}

static void slash_cmd_exit(ReplCtx *ctx, const char *args) {
  (void)args;
  ctx->should_exit = true;
}

static void slash_cmd_clear(ReplCtx *ctx, const char *args) {
  (void)ctx;
  (void)args;
  /* ANSI: move cursor home + clear screen + clear scrollback. The
   * 3J part is what makes Cmd-K / Ctrl-L equivalents actually wipe
   * the scrollback, not just the viewport. Harmless on terminals
   * that ignore it. */
  std::fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
  std::fflush(stdout);
}

static void slash_cmd_history(ReplCtx *ctx, const char *args) {
  (void)args;
  /* Isocline persists history at <data_dir>/.ai_session_history via
   * xLineSetHistory but doesn't expose a public enumeration API, so
   * the cheapest way to show the user their recall buffer is to
   * dump the file straight to stdout. If the file doesn't exist
   * yet (first run, no entries saved), say so rather than printing
   * a confusing blank. */
  if (!ctx->hist_path) {
    std::puts("(history not initialised)");
    return;
  }
  std::FILE *f = std::fopen(ctx->hist_path, "r");
  if (!f) {
    std::puts("(no history yet — submit a message and come back)");
    return;
  }
  char   buf[4096];
  size_t lines = 0;
  while (std::fgets(buf, sizeof(buf), f)) {
    /* xline stores one entry per line, sometimes prefixed with a
     * '#' comment line (v1.1 uses a plain-text format); we pass
     * everything through verbatim so the user sees exactly what
     * Ctrl-R would match against. */
    std::fputs(buf, stdout);
    if (buf[std::strlen(buf) - 1] != '\n') std::putchar('\n');
    ++lines;
  }
  std::fclose(f);
  std::printf("(%zu history line(s); Ctrl-R to search)\n", lines);
}

static void slash_cmd_tokens(ReplCtx *ctx, const char *args) {
  (void)args;
  std::printf("total_tokens=%d\n", ctx->total_tokens);
  if (ctx->budget_limit > 0) {
    std::printf("budget: remaining=%zu/%zu calibrator=%.3fx samples=%zu "
                "estimated=%zu",
                ctx->budget_remaining, ctx->budget_limit, ctx->budget_factor,
                ctx->budget_samples, ctx->budget_estimated);
    if (ctx->last_actual_prompt >= 0) {
      std::printf(" last_actual_prompt=%d", ctx->last_actual_prompt);
    }
    std::putchar('\n');
  } else {
    std::puts("budget: (no GatePassed event yet — submit a message first)");
  }
}

/* Dispatch a '/' line. Returns true if the line was a slash command
 * (whether or not it was recognised); the caller should then skip
 * the model submit path. Returns false for plain chat input.
 *
 * The caller passes the raw line — we don't mutate it, we just
 * isolate the command token (everything up to the first whitespace)
 * and the argument tail. Unknown commands print a hint instead of
 * silently falling through to the model, because "oops my /taht
 * typo just got sent to gpt-4o" is a worse UX than "unknown
 * command". */
static bool slash_dispatch(ReplCtx *ctx, const char *line) {
  if (!line || line[0] != '/') return false;
  /* Split command and args. */
  size_t cmd_len = 0;
  while (line[cmd_len] && line[cmd_len] != ' ' && line[cmd_len] != '\t')
    ++cmd_len;
  const char *args = line + cmd_len;
  while (*args == ' ' || *args == '\t') ++args;
  for (size_t i = 0; i < g_slash_cmds_count; ++i) {
    const SlashCmd *c = &g_slash_cmds[i];
    if (std::strlen(c->name) == cmd_len &&
        std::strncmp(c->name, line, cmd_len) == 0) {
      if (c->fn) c->fn(ctx, args);
      return true;
    }
  }
  std::printf("unknown command: %.*s  (try /help)\n", (int)cmd_len, line);
  return true;
}

/* ── REPL state ─────────────────────────────────────────────────────── */

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

static void on_text(xAiSession sess, const char *chunk, size_t len, void *ud) {
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
    std::fputs("\x1b[2m[thinking] ", stdout);
    ctx->in_thinking = true;
  }
  std::fwrite(chunk, 1, len, stdout);
  std::fflush(stdout);
}

static void on_tool(xAiSession sess, const char *tool_name, int started,
                    void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  if (!after_thinking) std::putchar('\n');
  std::printf("\x1b[2m[tool] %s %s\x1b[0m\n", tool_name ? tool_name : "(null)",
              started ? "starting" : "finished\n");
  std::fflush(stdout);
}

static void on_tool_output(xAiSession sess, const char *tool_use_id,
                           const char *tool_name, const char *data, size_t len,
                           void *ud) {
  (void)sess;
  (void)tool_use_id;
  (void)tool_name;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Close any open thinking block before showing tool output. */
  end_thinking(ctx);
  /* Render tool output faint with a visual prefix so it's distinct
   * from the model's final text reply. The prefix uses ⏎ to signal
   * "this is live output, not the answer". */
  std::fputs("\x1b[2m", stdout);
  std::fwrite(data, 1, len, stdout);
  std::fputs("\x1b[0m", stdout);
  std::fflush(stdout);
}

static void on_sidecar(xAiSession sess, xAiSidecarEvent event, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  std::fputs("\x1b[2m", stdout);
  switch (event) {
  case xAiSidecarEvent_Started:
    std::printf("[sidecar] analyzing idle tool...\n");
    break;
  case xAiSidecarEvent_Done:
    std::printf("[sidecar] done\n");
    break;
  }
  std::fputs("\x1b[0m", stdout);
  std::fflush(stdout);
}

static const char *done_reason_name(xAiDoneReason r) {
  switch (r) {
  case xAiDoneReason_Completed:
    return "completed";
  case xAiDoneReason_MaxTurns:
    return "max_turns";
  case xAiDoneReason_PromptTooLong:
    return "prompt_too_long";
  case xAiDoneReason_Aborted:
    return "aborted";
  case xAiDoneReason_ModelError:
    return "model_error";
  case xAiDoneReason_ToolError:
    return "tool_error";
  case xAiDoneReason_Stopped:
    return "stopped";
  }
  return "?";
}

static void on_done(xAiSession sess, xAiDoneReason reason,
                    const xAiUsage *usage, void *ud) {
  (void)sess;
  auto *ctx            = static_cast<ReplCtx *>(ud);
  bool  after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  /* [done] is chrome — render the whole line faint so it recedes and
   * the model's answer above stays visually primary. Extra blank
   * line after so the next `> ` prompt isn't glued to the status. */
  if (!after_thinking) std::putchar('\n');
  std::fputs("\x1b[2m", stdout);
  /* Compute elapsed time from user input to this done event. */
  double elapse = (xMonoMs() - ctx->input_ms) / 1000.0;
  std::printf("\n[done] reason=%s reply_bytes=%zu elapse=%.2fs",
              done_reason_name(reason), ctx->reply_bytes, elapse);
  /* Token accounting. prompt_tokens is the maximum across all
   * rounds (each round reports the full input size the provider
   * saw, so the last round's value is the total). completion_tokens
   * and total_tokens are additive per round. The provider fills -1
   * for fields it couldn't parse; we hide those so the line stays
   * clean for servers that only report a subset. A NULL usage means
   * the server never sent a usage object — rare in practice
   * (moonshot, openai, deepseek all support
   * stream_options.include_usage). */
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
      ctx->total_tokens += usage->total_tokens;
      std::printf(" total=%d", ctx->total_tokens);
    }
  }
  /* Context-budget snapshot. budget_remaining and budget_limit
   * are populated by the GatePassed event that fires at the start
   * of every round (before the Query is submitted). Displaying
   * remaining/limit gives the user a real-time view of how much
   * context headroom is left before the budget gate would trigger
   * TruncateOldest or SummarizeOldest. A remaining of 0 means
   * the very next input is likely to hit the cap.
   * calibrator_factor is the EWMA-smoothed multiplier that
   * bytes/4 gets scaled by; it starts at 1.0 and drifts toward
   * (actual_prompt_tokens / estimated_prompt_tokens). samples
   * is the count of accepted observations. est is the calibrated
   * pre-submit estimate for this round. */
  if (ctx->budget_limit > 0) {
    std::printf(" budget=%zu/%zu %.3fx samples=%zu est=%zu",
                ctx->budget_remaining, ctx->budget_limit, ctx->budget_factor,
                ctx->budget_samples, ctx->budget_estimated);
    if (ctx->last_actual_prompt >= 0) {
      std::printf(" actual=%d", ctx->last_actual_prompt);
    }
  }
  std::fputs("\x1b[0m\n\n", stdout);
  std::fflush(stdout);
  xEventLoopStop(ctx->loop);
}

static void on_error(xAiSession sess, xErrno err, const char *msg, void *ud) {
  (void)sess;
  auto *ctx    = static_cast<ReplCtx *>(ud);
  /* Same SGR hygiene as on_done — error might fire mid-thinking.
   * Errors are the one piece of chrome that should NOT recede — use
   * bold red (`\x1b[1;31m`) instead of faint so the user notices the
   * run failed at a glance. */
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  if (!after_thinking) std::fputc('\n', stderr);
  std::fprintf(stderr, "\x1b[1;31m[error] errno=%d msg=%s\x1b[0m\n", (int)err,
               msg ? msg : "(none)");
  /* Surface the budget gate explicitly. PromptTooLong is the one
   * errno most likely to surprise a demo user ("I didn't do
   * anything weird, why did my innocuous follow-up get rejected?")
   * — it means either the rolling history plus the incoming
   * message overflowed sconf.budget.max_tokens with no room to
   * trim below keep_recent_turns, or the incoming message alone
   * is bigger than the cap. The fix is almost always "raise the
   * cap" for a calibrator demo; production callers would
   * typically switch to SummarizeOldest or a Callback policy. */
  if (err == xErrno_PromptTooLong) {
    std::fprintf(stderr, "\x1b[1;31m        hit budget cap — raise "
                         "sconf.budget.max_tokens or lower "
                         "keep_recent_turns\x1b[0m\n");
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  xEventLoopStop(ctx->loop);
}

/* ── Budget-event callback ────────────────────────────────────────────
 *
 * Registered via sconf.budget.on_budget_event so the REPL user can
 * observe the SummarizeOldest / TruncateOldest lifecycle in real time.
 * All three events are informational; ignoring them doesn't change
 * session behaviour, but surfacing them makes the budget demo much
 * easier to follow — the user sees why a subsequent xAiSessionInput
 * returned Busy (Compacting) and knows when to retry (CompactDone). */
static void on_budget_event(xAiSession sess, xAiBudgetEvent event,
                            const void *info, void *ud) {
  (void)sess;
  auto *ctx            = static_cast<ReplCtx *>(ud);
  bool  after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  /* Budget events are chrome — render faint, same as [tool]/[done]. */
  if (!after_thinking) std::putchar('\n');
  std::fputs("\x1b[2m", stdout);
  switch (event) {
  case xAiBudgetEvent_Compacting: {
    auto *ci = static_cast<const xAiBudgetCompactInfo *>(info);
    std::printf("[budget] compacting %zu old entries...",
                ci ? ci->entries_compacted : 0);
    break;
  }
  case xAiBudgetEvent_CompactDone: {
    auto *cdi = static_cast<const xAiBudgetCompactDoneInfo *>(info);
    if (cdi && cdi->summary_ok) {
      std::printf("[budget] compact done — summary %zu tokens, "
                  "%zu entries affected",
                  cdi->summary_tokens, cdi->entries_affected);
    } else {
      std::printf(
        "[budget] compact degraded to truncate — %zu entries affected",
        cdi ? cdi->entries_affected : 0);
    }
    /* Compact is done — stop the event loop so the REPL's
     * xEventLoopRun returns and the auto-retry after Busy
     * can proceed. Without this the loop blocks forever
     * because the compact on_done path does NOT call
     * xEventLoopStop (it's an internal operation, not a
     * user-visible Query completion). */
    xEventLoopStop(ctx->loop);
    break;
  }
  case xAiBudgetEvent_Truncated: {
    auto *ti = static_cast<const xAiBudgetTruncateInfo *>(info);
    std::printf("[budget] truncated %zu old entries",
                ti ? ti->entries_removed : 0);
    break;
  }
  case xAiBudgetEvent_GatePassed: {
    auto *gi = static_cast<const xAiBudgetGateInfo *>(info);
    if (gi) {
      ctx->budget_limit       = gi->limit;
      ctx->budget_remaining   = gi->remaining;
      ctx->budget_factor      = gi->calibrator_factor;
      ctx->budget_samples     = gi->calibrator_samples;
      ctx->budget_estimated   = gi->estimated;
      ctx->last_actual_prompt = gi->last_first_round_prompt_tokens;
      std::printf("[budget] gate passed — remaining %zu/%zu tokens",
                  gi->remaining, gi->limit);
    }
    break;
  }
  }
  std::fputs("\x1b[0m\n", stdout);
  std::fflush(stdout);
}

/* ── Main ───────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  /* ── Parse command-line options ─────────────────────────────────── */
  int         opt;
  const char *data_dir_arg = nullptr;
  while ((opt = getopt(argc, argv, "d:h")) != -1) {
    switch (opt) {
    case 'd':
      data_dir_arg = optarg;
      break;
    case 'h':
    default:
      std::fprintf(stderr, "Usage: %s [-d <path>]\n", argv[0]);
      return 1;
    }
  }

  /* Default data_dir to the current working directory. */
  char        cwd_buf[4096];
  const char *data_dir = data_dir_arg;
  if (!data_dir) {
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
      data_dir = cwd_buf;
    } else {
      data_dir = ".";
    }
  }

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

  /* ── Tools ─────────────────────────────────────────────────────────── */

  xAiShellConf shell_conf;
  std::memset(&shell_conf, 0, sizeof(shell_conf));
  shell_conf.on_command = [](const char *command, const char *cwd, void *) {
    if (cwd && cwd[0]) {
      std::printf("\x1b[2m  $ (cd %s && %s)\x1b[0m\n", cwd, command);
    } else {
      std::printf("\x1b[2m  $ %s\x1b[0m\n", command);
    }
    std::fflush(stdout);
  };
  shell_conf.on_result = [](int exit_code, size_t stdout_len, size_t stderr_len,
                            int timed_out, void *) {
    std::printf("\x1b[2m  exit=%d stdout=%zu stderr=%zu%s\x1b[0m\n", exit_code,
                stdout_len, stderr_len, timed_out ? " (timed out)" : "");
    std::fflush(stdout);
  };
  xAiTool shell_tool = xAiToolShellCreate(loop, &shell_conf);
  if (!shell_tool) {
    std::fprintf(stderr, "failed to create shell tool\n");
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  const xAiTool   *tool_ptrs[] = {&shell_tool};
  const size_t TOTAL_TOOLS = 1;
  /* ── Session config (agent's default session) ──────────────────────
   *
   * Instead of creating a session manually and managing its
   * lifecycle, we set default_session_conf on the agent so it
   * creates a built-in default session at construction time.
   * The session is retrieved via xAiAgentDefaultSession() and
   * is destroyed automatically by xAiAgentDestroy(). */
  ReplCtx ctx;
  ctx.loop = loop;

  xAiSessionConf sconf;
  std::memset(&sconf, 0, sizeof(sconf));
  sconf.cbs.on_text        = on_text;
  sconf.cbs.on_thinking    = on_thinking;
  sconf.cbs.on_tool        = on_tool;
  sconf.cbs.on_tool_output = on_tool_output;
  sconf.cbs.on_sidecar    = on_sidecar;
  sconf.cbs.on_done        = on_done;
  sconf.cbs.on_error       = on_error;
  sconf.cbs.user_data      = &ctx;

  /* Opt into the structured budget pipeline so the calibrator
   * actually runs. Without a non-Disabled policy the gate short-
   * circuits, last_prompt_estimate stays zero, and on_done's
   * calibrator update bails out — factor would forever read 1.0
   * and samples 0, defeating the whole point of this demo.
   *
   * 8192 was picked empirically: large enough that a single
   * long-form answer (think: a derivation with multi-paragraph
   * reasoning) plus the floor pinned by keep_recent_turns won't
   * trip the gate on turn #2, but small enough that a handful of
   * sustained turns will eventually push the rolling history
   * past the cap and exercise TruncateOldest. keep_recent_turns
   * =2 is the floor — the current user turn and the immediately
   * prior assistant turn are never discarded, so the model keeps
   * local context even when the trimmer fires. If you shrink
   * max_tokens below ~4096 expect xErrno_PromptTooLong (which the
   * REPL and on_error both surface with a hint line below), and
   * see session.c's keep_recent_turns floor logic for why. */
  sconf.budget.policy            = xAiBudgetPolicy_Auto;
  sconf.budget.max_tokens        = 8192;
  sconf.budget.keep_recent_turns = 2;
  sconf.budget.on_budget_event   = on_budget_event;
  sconf.budget.budget_event_ud   = &ctx;

  /* Sidecar idle timeout: when an async tool (e.g. shell) has not
   * produced output for 3 seconds, launch a sidecar Query so the
   * AI can inspect the situation and decide what to do next. Zero
   * would disable the sidecar mechanism entirely. */
  sconf.sidecar_idle_ms = 3000;

  /* ── Agent ──────────────────────────────────────────────────────── */
  xAiAgentConf aconf;
  std::memset(&aconf, 0, sizeof(aconf));
  aconf.loop     = loop;
  aconf.provider = pvd;
  aconf.model    = model;
  aconf.system_prompt =
    "You are a concise assistant running on xKit's xai session "
    "demo. You have access to a shell tool that can execute "
    "commands via /bin/sh -c and return stdout/stderr/exit code. "
    "Use it when you need to run commands, check the system, or "
    "compute things. You may chain multiple tool calls in a single "
    "turn. Keep replies short.";
  aconf.tools                = tool_ptrs;
  aconf.tools_count          = TOTAL_TOOLS;
  aconf.max_turns            = 64;
  aconf.agent_id             = "test";
  aconf.data_dir             = data_dir;
  aconf.enable_sidecar_query = 1;
  aconf.default_session_conf = &sconf;

  xAiAgent agent = xAiAgentCreate(&aconf);
  if (!agent) {
    std::fprintf(stderr, "failed to create agent\n");
    xAiToolDestroy(shell_tool);
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* Retrieve the agent's built-in default session — no manual
   * create/destroy needed. The session lives for the agent's
   * entire lifetime. */
  xAiSession sess = xAiAgentDefaultSession(agent);
  if (!sess) {
    std::fprintf(stderr, "agent has no default session\n");
    xAiAgentDestroy(agent);
    xAiToolDestroy(shell_tool);
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  ctx.sess = sess;

  std::printf("xai session REPL (model: %s)\n", model);
  std::printf("Type a message and press Enter. Type '/' + Tab for commands, "
              "or /exit to quit.\n"
              "Registered tools: shell\n\n");

  /* ── Line editor (xline) ──────────────────────────────────────────
   *
   * Persist history under the agent's data_dir so each agent
   * namespace keeps its own recall buffer. 1000 entries is plenty
   * for a chat REPL — isocline prunes oldest on overflow. Passing
   * NULL would disable persistence (history kept only in-memory);
   * passing a path enables both load-on-start and save-on-exit. */
  char hist_path[4096];
  std::snprintf(hist_path, sizeof(hist_path), "%s/.ai_session_history",
                data_dir);
  xLineSetHistory(hist_path, 1000);
  ctx.hist_path = hist_path;

  /* Slash-command completion. isocline calls slash_completer on Tab;
   * when the current prefix resolves to a single match it ALSO shows
   * the tail as an inline faint hint (fish-shell style) without
   * needing Tab. Hints are on by default, but we set them explicitly
   * so a future xLineEnableHint(false) somewhere else doesn't silently
   * break the UX. completion_preview (the faint candidate shown while
   * the menu is open) is already on by default; left as-is. */
  xLineSetDefaultCompleter(slash_completer, nullptr);
  xLineEnableHint(true);
  /* Default hint delay is 500ms — fine for typing prose where you
   * don't want a flash every keystroke, but annoying when you've
   * just typed `/c` and know `clear` is coming. Zero delay makes
   * the inline hint feel instant, matching fish-shell / zsh-
   * autosuggestions UX. Hints still only appear when the current
   * prefix resolves to a single candidate, so ambiguous prefixes
   * (`/h` — /help vs /history) stay quiet until you type more or
   * hit Tab to see the menu. */
  xLineSetHintDelay(0);

  while (true) {
    /* xLineReadline prints its own prompt (with styling support) and
     * returns a heap-allocated UTF-8 string that the caller must
     * free via xLineFree. Returns NULL on EOF (Ctrl-D on an empty
     * line) — we treat that like `exit`. */
    char *line = xLineReadline("");
    if (!line) break;

    size_t len = std::strlen(line);
    if (len == 0) {
      xLineFree(line);
      continue;
    }
    /* Slash commands are intercepted locally — they never reach the
     * model. Legacy `exit` / `quit` bare words remain as a courtesy
     * for muscle memory, but `/exit` is the documented spelling. */
    if (std::strcmp(line, "exit") == 0 || std::strcmp(line, "quit") == 0) {
      xLineHistoryRemoveLast();
      xLineFree(line);
      break;
    }
    if (line[0] == '/') {
      /* Don't pollute history with command chrome — slash commands
       * are pure REPL actions, not chat input. */
      xLineHistoryRemoveLast();
      slash_dispatch(&ctx, line);
      xLineFree(line);
      if (ctx.should_exit) break;
      continue;
    }

    ctx.saw_first_delta = false;
    ctx.in_thinking     = false;
    ctx.reply_bytes     = 0;
    ctx.input_ms        = xMonoMs();

    /* xAiMessageFromText creates a User-role borrow-view that points
     * at `line` via a thread-local content slot (see message.c).
     * xAiSessionInput duplicates every byte into session-owned
     * memory before it returns, so it's safe to free `line`
     * immediately after the (possibly-retried) submit completes. */
    xAiMessage m   = xAiMessageFromText(line);
    xErrno     err = xAiSessionInput(sess, m);
    while (err == xErrno_Busy) {
      /* Busy from SummarizeOldest is expected — the on_budget_event
       * callback already printed "[budget] compacting ...". Run the
       * event loop so the compact Query can complete; on_budget_event
       * will print "[budget] compact done" and then we auto-retry
       * the user's input. If Busy is from a regular Query still in
       * flight (shouldn't happen in this single-flight REPL), the
       * loop returns immediately and we report it below.
       *
       * Loop (not single retry): after a compact that degraded to
       * TruncateOldest, the session may still be over budget, and
       * session_enforce_budget_ can launch another compact round.
       * Each Busy return means "an async operation is in flight";
       * we wait for it to finish and retry until the gate lets the
       * message through or returns a non-Busy error. */
      xEventLoopRun(loop);
      /* Compact done — session is idle now, retry the input. */
      err = xAiSessionInput(sess, m);
    }
    if (err != xErrno_Ok) {
      /* Synchronous rejection path: the gate fires before the
       * Query is even handed off, so on_error never runs. Mirror
       * the PromptTooLong hint from on_error here so the same
       * advice reaches users regardless of which path triggered.
       * Other errnos (Busy, InvalidState) have no budget-side
       * remedy, so they just print the bare code. */
      std::fprintf(stderr, "[error] input rejected (errno=%d)\n", (int)err);
      if (err == xErrno_PromptTooLong) {
        std::fprintf(stderr, "        hit budget cap — raise "
                             "sconf.budget.max_tokens or lower "
                             "keep_recent_turns\n");
      }
      xLineFree(line);
      continue;
    }

    xEventLoopRun(loop);
    xLineFree(line);
  }

  std::printf("\nBye!\n");

  /* No xAiSessionDestroy needed — the default session is owned
   * by the agent and destroyed automatically in xAiAgentDestroy. */
  xAiAgentDestroy(agent);
  for (size_t i = 0; i < TOTAL_TOOLS; ++i)
    xAiToolDestroy(*tool_ptrs[i]);
  xAiProviderDestroy(pvd);
  xHttpClientDestroy(http);
  xEventLoopDestroy(loop);
  return 0;
}
