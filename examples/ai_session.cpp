/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ai_session.cpp - Streaming REPL driven by the full xagent stack
 *                  (xAgent + xAgentSession + xAgentTool + xAgentProvider).
 *
 * Unlike ai_openai.cpp (which drives the provider vtable directly for
 * end-to-end diagnostics), this demo is the canonical integration
 * path that user code should copy:
 *
 *   xAgentProvider -> xAgent -> xAgentSession
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

#include <xagent/agent.h>
#include <xagent/message.h>
#include <xagent/provider.h>
#include <xagent/provider_openai.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xagent/tool_shell.h>
#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xbase/time.h>
#include <xhttp/client.h>
#include <xline/line.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <unistd.h>

/* ── REPL state ─────────────────────────────────────────────────────── */
struct ReplCtx {
  xEventLoop   loop             = nullptr;
  xAgentSession   sess             = nullptr;
  xLineHandle  line             = nullptr; /* current async editor */
  xEventSource src              = nullptr; /* loop fd registration */
  bool         busy             = false;   /* AI run in flight */
  bool         pending_retry    = false; /* retry pending_text after compact */
  char        *pending_text     = nullptr; /* stashed submit text, owned */
  bool         saw_first_delta  = false;
  bool         in_thinking      = false; /* currently streaming thinking? */
  size_t       reply_bytes      = 0;
  int          total_tokens     = 0;   /* cumulative across all rounds */
  size_t       budget_limit     = 0;   /* from last GatePassed event */
  size_t       budget_remaining = 0;   /* from last GatePassed event */
  double       budget_factor    = 1.0; /* EWMA calibrator factor */
  size_t       budget_samples   = 0;   /* calibrator observation count */
  size_t       budget_estimated = 0;   /* calibrated pre-submit estimate */
  int last_actual_prompt = -1; /* provider-reported first-round prompt_tokens */
  uint64_t    input_ms   = 0;  /* monotonic timestamp (ms) at user input */
  bool        should_exit = false;   /* set by /exit handler */
  const char *hist_path   = nullptr; /* xline history file, for /history */
};

/* ── Output helpers ───────────────────────────────────────────────────
 *
 * All AI-driven output must go through xLinePrintAbove / Chunk so the
 * user's prompt row stays intact while the model streams. The editor
 * is kept alive for the entire lifetime of the REPL (including during
 * AI runs), which is what lets the user type slash commands — notably
 * /cancel — while the model is thinking or streaming.
 *
 * above_printf() formats into a stack buffer then delegates to
 * xLinePrintAbove (which appends a trailing '\n' if missing) so the
 * call site reads like a plain std::printf. For long-running text
 * streams we use xLinePrintAboveChunk directly to preserve the
 * "token-by-token" visual cadence without forcing a newline. */
static void above_printf(xLineHandle h, const char *fmt, ...) {
  if (!h) return;
  char    buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  xLinePrintAbove(h, buf);
}

static void above_chunk(xLineHandle h, const char *s, size_t len) {
  if (!h || !s || len == 0) return;
  /* xLinePrintAboveChunk requires a NUL-terminated C string. Copy into
   * a stack buffer when the fragment fits; fall back to heap for the
   * rare oversized token. */
  char  stack[512];
  char *p = stack;
  if (len + 1 > sizeof(stack)) {
    p = (char *)std::malloc(len + 1);
    if (!p) return;
  }
  std::memcpy(p, s, len);
  p[len] = '\0';
  xLinePrintAboveChunk(h, p);
  if (p != stack) std::free(p);
}

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
  const char  *name; /* including leading '/' */
  const char  *help;
  SlashCmdFunc fn;
};

static void slash_cmd_help(ReplCtx *ctx, const char *args);
static void slash_cmd_exit(ReplCtx *ctx, const char *args);
static void slash_cmd_clear(ReplCtx *ctx, const char *args);
static void slash_cmd_history(ReplCtx *ctx, const char *args);
static void slash_cmd_tokens(ReplCtx *ctx, const char *args);
static void slash_cmd_cancel(ReplCtx *ctx, const char *args);

static const SlashCmd g_slash_cmds[] = {
  {"/help", "show this help", slash_cmd_help},
  {"/exit", "quit the REPL", slash_cmd_exit},
  {"/clear", "clear the terminal screen", slash_cmd_clear},
  {"/history", "print input history", slash_cmd_history},
  {"/tokens", "show cumulative token usage", slash_cmd_tokens},
  {"/cancel", "interrupt the active AI run", slash_cmd_cancel},
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
  if (len != 1) return false; /* ASCII only; no multi-byte in cmd names */
  char c = s[0];
  return c == '/' || c == '_' || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/* Inner completer invoked by xLineCompleteWord with the already-
 * extracted token. Because we went through xLineCompleteWord, every
 * xLineAddCompletion* call here is treated as a *replacement* for
 * `prefix` — xline handles the delete-before bookkeeping, so we
 * just hand it the full command name. */
static void slash_completer_inner(xLineCompletionEnv cenv, const char *prefix) {
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
static void slash_completer(xLineCompletionEnv cenv, const char *prefix) {
  (void)prefix; /* xLineCompleteWord re-derives it using our predicate */
  xLineCompleteWord(cenv, prefix, slash_completer_inner, is_slash_cmd_char);
}

static void slash_cmd_help(ReplCtx *ctx, const char *args) {
  (void)args;
  /* Command list goes into the below panel so it stays pinned above
   * the prompt across model streaming. Keep the panel tight — every
   * row here permanently eats into the above region's visible budget
   * (and on short terminals an oversize panel will push earlier
   * above output into scrollback). Usage hints are one-shot
   * information, so print them through the above channel where they
   * scroll naturally with the rest of the conversation. */
  std::string body;
  for (size_t i = 0; i < g_slash_cmds_count; ++i) {
    char line[128];
    std::snprintf(line, sizeof(line), "  %-10s %s", g_slash_cmds[i].name,
                  g_slash_cmds[i].help);
    if (!body.empty()) body.push_back('\n');
    body.append(line);
  }
  /* title suppressed: the body already reads as a help listing and the
   * "/help" tab on top just stole a row. */
  xLineSetBelowPanel(ctx->line, nullptr, body.c_str());
  xLinePrintAbove(ctx->line,
                  "\x1b[2mAnything not starting with '/' is sent to the "
                  "model. Type '/' + Tab to browse commands.\x1b[0m");
}

static void slash_cmd_exit(ReplCtx *ctx, const char *args) {
  (void)args;
  ctx->should_exit = true;
}

static void slash_cmd_clear(ReplCtx *ctx, const char *args) {
  (void)args;
  (void)ctx;
  /* ANSI: move cursor home + clear screen + clear scrollback. The
   * 3J part is what makes Cmd-K / Ctrl-L equivalents actually wipe
   * the scrollback, not just the viewport. Harmless on terminals
   * that ignore it. Goes through the editor's "above" channel so the
   * prompt survives the wipe cleanly. Drop any below panel too so
   * the freshly cleared viewport isn't cluttered with stale output. */
  xLineClearBelowPanel(ctx->line);
  xLinePrintAbove(ctx->line, "\x1b[H\x1b[2J\x1b[3J");
}

static void slash_cmd_cancel(ReplCtx *ctx, const char *args) {
  (void)args;
  if (!ctx->busy) {
    above_printf(ctx->line, "\x1b[2m(no AI run in flight)\x1b[0m");
    return;
  }
  /* xAgentSessionCancel is async: it asks the provider to unwind and
   * on_done is still delivered (with reason == Aborted). Flip busy
   * off there, not here — that keeps on_text/on_done ordering
   * consistent with the natural-completion path. */
  above_printf(ctx->line, "\x1b[2m[cancel] aborting run…\x1b[0m");
  xAgentSessionCancel(ctx->sess);
}

static void slash_cmd_history(ReplCtx *ctx, const char *args) {
  (void)args;
  /* Isocline persists history at <data_dir>/.ai_session_history via
   * xLineSetHistory but doesn't expose a public enumeration API, so
   * the cheapest way to show the user their recall buffer is to
   * dump the file line by line. Pinning the result into the below
   * panel means the listing sticks around across model streaming
   * and the user can grep it visually while typing a new prompt. */
  if (!ctx->hist_path) {
    xLineSetBelowPanel(ctx->line, nullptr, "(history not initialised)");
    return;
  }
  std::FILE *f = std::fopen(ctx->hist_path, "r");
  if (!f) {
    xLineSetBelowPanel(
      ctx->line, nullptr,
      "(no history yet \u2014 submit a message and come back)");
    return;
  }
  std::string body;
  char        buf[4096];
  size_t      lines = 0;
  while (std::fgets(buf, sizeof(buf), f)) {
    size_t n = std::strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
    if (!body.empty()) body.push_back('\n');
    body.append(buf);
    ++lines;
  }
  std::fclose(f);
  if (body.empty()) body = "(empty)";
  char tail[64];
  std::snprintf(tail, sizeof(tail), "\n\n(%zu line(s); Ctrl-R to search)",
                lines);
  body.append(tail);
  xLineSetBelowPanel(ctx->line, nullptr, body.c_str());
}
static void slash_cmd_tokens(ReplCtx *ctx, const char *args) {
  (void)args;
  std::string body;
  char        buf[256];
  std::snprintf(buf, sizeof(buf), "total_tokens=%d", ctx->total_tokens);
  body.append(buf);
  if (ctx->budget_limit > 0) {
    char extra[64];
    extra[0] = '\0';
    if (ctx->last_actual_prompt >= 0) {
      std::snprintf(extra, sizeof(extra), " last_actual_prompt=%d",
                    ctx->last_actual_prompt);
    }
    std::snprintf(buf, sizeof(buf),
                  "\nbudget: remaining=%zu/%zu calibrator=%.3fx samples=%zu "
                  "estimated=%zu%s",
                  ctx->budget_remaining, ctx->budget_limit, ctx->budget_factor,
                  ctx->budget_samples, ctx->budget_estimated, extra);
    body.append(buf);
  } else {
    body.append(
      "\nbudget: (no GatePassed event yet \u2014 submit a message first)");
  }
  xLineSetBelowPanel(ctx->line, nullptr, body.c_str());
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
  while (*args == ' ' || *args == '\t')
    ++args;
  for (size_t i = 0; i < g_slash_cmds_count; ++i) {
    const SlashCmd *c = &g_slash_cmds[i];
    if (std::strlen(c->name) == cmd_len &&
        std::strncmp(c->name, line, cmd_len) == 0) {
      if (c->fn) c->fn(ctx, args);
      return true;
    }
  }
  above_printf(ctx->line, "unknown command: %.*s  (try /help)", (int)cmd_len,
               line);
  return true;
}

/* ── AI callbacks (all output goes through xline's above channel) ──── */

/* Close an open thinking block: reset SGR (`\x1b[0m`) + a blank row so
 * whatever follows (final text, [tool], [done], ...) has visual
 * breathing room. The trailing blank is important when thinking ends
 * with a sentence that looks like a reply ("简短回复：..." etc) —
 * without it the eye merges the faint scratchpad into the bright
 * answer. Must be called before printing anything that shouldn't
 * inherit faint style. No-op if no thinking block is open, so it's
 * safe to sprinkle liberally. */
static void end_thinking(ReplCtx *ctx) {
  if (!ctx->in_thinking) return;
  /* Two newlines: one to finish the trailing line, one for the blank
   * separator row. xLinePrintAbove will add another trailing '\n' if
   * the argument doesn't already end with one (it does, so no-op). */
  xLinePrintAbove(ctx->line, "\x1b[0m\n");
  ctx->in_thinking = false;
}

static void on_text(xAgentSession sess, const char *chunk, size_t len, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Close the thinking block (if any) before the visible reply
   * starts, so they don't visually merge AND the terminal doesn't
   * stay in faint mode for the rest of the output. */
  if (ctx->in_thinking) {
    end_thinking(ctx);
    ctx->saw_first_delta = true;
  } else if (!ctx->saw_first_delta) {
    /* No-op: xLinePrintAboveChunk naturally starts on a fresh row
     * above the prompt, so we don't need the leading '\n' the
     * blocking version used. */
    ctx->saw_first_delta = true;
  }
  above_chunk(ctx->line, chunk, len);
  ctx->reply_bytes += len;
}

/* Thinking stream: dim + prefix so it's obviously "model scratchpad"
 * and not the final answer. ANSI 2 = faint; most modern terminals
 * honour it (including macOS Terminal and iTerm2). On the rare
 * terminal that doesn't, the `[thinking]` prefix still telegraphs
 * intent. */
static void on_thinking(xAgentSession sess, const char *chunk, size_t len,
                        void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  if (!ctx->in_thinking) {
    /* Open a new thinking block on its own line. */
    xLinePrintAboveChunk(ctx->line, "\x1b[2m[thinking] ");
    ctx->in_thinking = true;
  }
  above_chunk(ctx->line, chunk, len);
}

static void on_tool(xAgentSession sess, const char *tool_name, int started,
                    void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  above_printf(ctx->line, "\x1b[2m[tool] %s %s\x1b[0m",
               tool_name ? tool_name : "(null)",
               started ? "starting" : "finished");
}

static void on_tool_output(xAgentSession sess, const char *tool_use_id,
                           const char *tool_name, const char *data, size_t len,
                           void *ud) {
  (void)sess;
  (void)tool_use_id;
  (void)tool_name;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Close any open thinking block before showing tool output. */
  end_thinking(ctx);
  /* Render tool output faint; stream it as a chunk so multi-line
   * output doesn't force artificial breaks. The surrounding SGR
   * pair keeps the faint style scoped to the tool payload. */
  xLinePrintAboveChunk(ctx->line, "\x1b[2m");
  above_chunk(ctx->line, data, len);
  xLinePrintAboveChunk(ctx->line, "\x1b[0m");
}

static void on_sidecar(xAgentSession sess, xAgentSidecarEvent event, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  switch (event) {
  case xAgentSidecarEvent_Started:
    above_printf(ctx->line, "\x1b[2m[sidecar] analyzing idle tool...\x1b[0m");
    break;
  case xAgentSidecarEvent_Done:
    above_printf(ctx->line, "\x1b[2m[sidecar] done\x1b[0m");
    break;
  }
}

static const char *done_reason_name(xAgentDoneReason r) {
  switch (r) {
  case xAgentDoneReason_Completed:
    return "completed";
  case xAgentDoneReason_MaxTurns:
    return "max_turns";
  case xAgentDoneReason_PromptTooLong:
    return "prompt_too_long";
  case xAgentDoneReason_Aborted:
    return "aborted";
  case xAgentDoneReason_ModelError:
    return "model_error";
  case xAgentDoneReason_ToolError:
    return "tool_error";
  case xAgentDoneReason_Stopped:
    return "stopped";
  }
  return "?";
}

static void on_done(xAgentSession sess, xAgentDoneReason reason,
                    const xAgentUsage *usage, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  /* [done] is chrome — render the whole block faint so it recedes
   * and the model's answer above stays visually primary. Build the
   * full status line in a local buffer and emit it as one Above
   * call so the prompt repaints exactly once at the end. */
  char   line_buf[512];
  size_t off    = 0;
  double elapse = (xMonoMs() - ctx->input_ms) / 1000.0;
  off += std::snprintf(line_buf + off, sizeof(line_buf) - off,
                       "\x1b[2m\n[done] reason=%s reply_bytes=%zu elapse=%.2fs",
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
    off += std::snprintf(line_buf + off, sizeof(line_buf) - off, " tokens=");
    if (usage->prompt_tokens >= 0) {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "%d",
                           usage->prompt_tokens);
    } else {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "?");
    }
    off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "/");
    if (usage->completion_tokens >= 0) {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "%d",
                           usage->completion_tokens);
    } else {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "?");
    }
    if (usage->total_tokens >= 0) {
      ctx->total_tokens += usage->total_tokens;
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, " total=%d",
                           ctx->total_tokens);
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
    off += std::snprintf(line_buf + off, sizeof(line_buf) - off,
                         " budget=%zu/%zu %.3fx samples=%zu est=%zu",
                         ctx->budget_remaining, ctx->budget_limit,
                         ctx->budget_factor, ctx->budget_samples,
                         ctx->budget_estimated);
    if (ctx->last_actual_prompt >= 0) {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, " actual=%d",
                           ctx->last_actual_prompt);
    }
  }
  std::snprintf(line_buf + off, sizeof(line_buf) - off, "\x1b[0m\n");
  xLinePrintAbove(ctx->line, line_buf);

  ctx->busy = false;
  /* Natural completion of the user's run — nothing more to do.
   * The event loop keeps running so the editor stays interactive. */
}

static void on_error(xAgentSession sess, xErrno err, const char *msg, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Same SGR hygiene as on_done — error might fire mid-thinking.
   * Errors are the one piece of chrome that should NOT recede — use
   * bold red (`\x1b[1;31m`) instead of faint so the user notices the
   * run failed at a glance. */
  end_thinking(ctx);
  above_printf(ctx->line, "\x1b[1;31m[error] errno=%d msg=%s\x1b[0m", (int)err,
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
    above_printf(ctx->line, "\x1b[1;31m        hit budget cap — raise "
                            "sconf.budget.max_tokens or lower "
                            "keep_recent_turns\x1b[0m");
  }
  ctx->busy = false;
}

/* Forward declaration: on_budget_event needs to resubmit the
 * pending text after a compact completes. */
static xErrno repl_submit_text(ReplCtx *ctx, const char *text);

/* ── Budget-event callback ────────────────────────────────────────────
 *
 * Registered via sconf.budget.on_budget_event so the REPL user can
 * observe the SummarizeOldest / TruncateOldest lifecycle in real time.
 * All three events are informational; ignoring them doesn't change
 * session behaviour, but surfacing them makes the budget demo much
 * easier to follow — the user sees why a subsequent xAgentSessionInput
 * returned Busy (Compacting) and knows when to retry (CompactDone). */
static void on_budget_event(xAgentSession sess, xAgentBudgetEvent event,
                            const void *info, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  /* Budget events are chrome — render faint, same as [tool]/[done]. */
  switch (event) {
  case xAgentBudgetEvent_Compacting: {
    auto *ci = static_cast<const xAgentBudgetCompactInfo *>(info);
    above_printf(ctx->line,
                 "\x1b[2m[budget] compacting %zu old entries...\x1b[0m",
                 ci ? ci->entries_compacted : (size_t)0);
    break;
  }
  case xAgentBudgetEvent_CompactDone: {
    auto *cdi = static_cast<const xAgentBudgetCompactDoneInfo *>(info);
    if (cdi && cdi->summary_ok) {
      above_printf(ctx->line,
                   "\x1b[2m[budget] compact done — summary %zu tokens, "
                   "%zu entries affected\x1b[0m",
                   cdi->summary_tokens, cdi->entries_affected);
    } else {
      above_printf(ctx->line,
                   "\x1b[2m[budget] compact degraded to truncate — %zu "
                   "entries affected\x1b[0m",
                   cdi ? cdi->entries_affected : (size_t)0);
    }
    /* Compact finished — the session is idle now, so auto-retry the
     * pending user message. Unlike the blocking version, we never
     * stop the event loop: the editor stays live the whole time
     * and the retry just re-enters the input path. */
    if (ctx->pending_retry && ctx->pending_text) {
      char *text         = ctx->pending_text;
      ctx->pending_text  = nullptr;
      ctx->pending_retry = false;
      (void)repl_submit_text(ctx, text);
      std::free(text);
    }
    break;
  }
  case xAgentBudgetEvent_Truncated: {
    auto *ti = static_cast<const xAgentBudgetTruncateInfo *>(info);
    above_printf(ctx->line, "\x1b[2m[budget] truncated %zu old entries\x1b[0m",
                 ti ? ti->entries_removed : (size_t)0);
    break;
  }
  case xAgentBudgetEvent_GatePassed: {
    auto *gi = static_cast<const xAgentBudgetGateInfo *>(info);
    if (gi) {
      ctx->budget_limit       = gi->limit;
      ctx->budget_remaining   = gi->remaining;
      ctx->budget_factor      = gi->calibrator_factor;
      ctx->budget_samples     = gi->calibrator_samples;
      ctx->budget_estimated   = gi->estimated;
      ctx->last_actual_prompt = gi->last_first_round_prompt_tokens;
      above_printf(ctx->line,
                   "\x1b[2m[budget] gate passed — remaining %zu/%zu "
                   "tokens\x1b[0m",
                   gi->remaining, gi->limit);
    }
    break;
  }
  }
}

/* ── Async REPL glue ──────────────────────────────────────────────────
 *
 * The editor is kept alive for the whole REPL lifetime: users may
 * still edit and execute slash commands (notably /cancel) while the
 * AI run is in flight. When the user presses Enter we briefly close
 * the session so the line dispatch code can run without fighting the
 * edit region, then reopen a fresh session right after — this mirrors
 * xline_async.c's pattern. AI callbacks (on_text, on_tool, on_done,
 * …) write through xLinePrintAbove/Chunk so the prompt row stays
 * intact regardless of what the model is doing. */

static void repl_line_cb(int fd, xEventMask mask, void *arg);

static int repl_open_line(ReplCtx *ctx) {
  ctx->line = xLineBegin("");
  if (!ctx->line) {
    std::fprintf(stderr,
                 "xLineBegin failed (dumb tty? another session live?)\n");
    return -1;
  }
  int fd = xLineFd(ctx->line);
  if (fd < 0) {
    std::fprintf(stderr,
                 "xLineFd returned %d — not pollable on this platform\n", fd);
    xLineEnd(ctx->line);
    ctx->line = nullptr;
    return -1;
  }
  ctx->src = xEventAdd(ctx->loop, fd, xEvent_Read, repl_line_cb, ctx);
  if (!ctx->src) {
    std::fprintf(stderr, "xEventAdd failed for tty fd=%d\n", fd);
    xLineEnd(ctx->line);
    ctx->line = nullptr;
    return -1;
  }
  return 0;
}

static void repl_close_line(ReplCtx *ctx) {
  if (ctx->src) {
    xEventDel(ctx->loop, ctx->src);
    ctx->src = nullptr;
  }
  if (ctx->line) {
    xLineEnd(ctx->line);
    ctx->line = nullptr;
  }
}

/* Submit a user message to the session. Handles the Busy → compact
 * → retry dance asynchronously: if the gate returns Busy we stash
 * the text and wait for on_budget_event(CompactDone) to resubmit.
 * xAgentMessageFromText returns a thread-local borrow-view backed by
 * caller-owned storage, so we must keep the text alive until the
 * session has actually accepted (and internally duplicated) it —
 * which, on the Ok path, happens before xAgentSessionInput returns. */
static xErrno repl_submit_text(ReplCtx *ctx, const char *text) {
  ctx->saw_first_delta = false;
  ctx->in_thinking     = false;
  ctx->reply_bytes     = 0;
  ctx->input_ms        = xMonoMs();

  xAgentMessage m   = xAgentMessageFromText(text);
  xErrno     err = xAgentSessionInput(ctx->sess, m);
  if (err == xErrno_Busy) {
    /* A budget compact is in flight. Stash a copy of the text
     * (the caller's buffer may be freed before CompactDone fires)
     * and let on_budget_event re-enter submit on our behalf. */
    if (ctx->pending_text) std::free(ctx->pending_text);
    ctx->pending_text  = strdup(text);
    ctx->pending_retry = true;
    above_printf(ctx->line,
                 "\x1b[2m(session busy — will resubmit after compact)\x1b[0m");
    return xErrno_Busy;
  }
  if (err != xErrno_Ok) {
    above_printf(ctx->line,
                 "\x1b[1;31m[error] input rejected (errno=%d)\x1b[0m",
                 (int)err);
    if (err == xErrno_PromptTooLong) {
      above_printf(ctx->line, "\x1b[1;31m        hit budget cap — raise "
                              "sconf.budget.max_tokens or lower "
                              "keep_recent_turns\x1b[0m");
    }
    return err;
  }
  ctx->busy = true;
  return xErrno_Ok;
}

/* Decide what to do with a completed line. Slash commands are
 * intercepted locally; chat input is submitted to the session iff
 * no run is currently active. Returning non-zero asks the caller
 * to stop the REPL. */
static int repl_handle_line(ReplCtx *ctx, char *line) {
  if (!line) return 0;
  size_t len = std::strlen(line);
  if (len == 0) return 0;

  /* Legacy `exit` / `quit` bare words remain as a courtesy for
   * muscle memory, but `/exit` is the documented spelling. */
  if (std::strcmp(line, "exit") == 0 || std::strcmp(line, "quit") == 0) {
    xLineHistoryRemoveLast();
    return 1;
  }
  if (line[0] == '/') {
    /* Don't pollute history with command chrome. */
    xLineHistoryRemoveLast();
    slash_dispatch(ctx, line);
    return ctx->should_exit ? 1 : 0;
  }

  if (ctx->busy) {
    /* The AI is still working; reject the submit but keep the
     * entry in history so the user can Up-arrow and resend once
     * /cancel (or on_done) clears the flag. */
    above_printf(ctx->line,
                 "\x1b[33m(AI is busy \u2014 use /cancel to interrupt, then "
                 "resend with Up-arrow)\x1b[0m");
    return 0;
  }

  /* Real chat submit: sweep away any slash-command panel from a
   * previous turn so the new conversation doesn't carry stale UI. */
  xLineClearBelowPanel(ctx->line);
  (void)repl_submit_text(ctx, line);
  return 0;
}
static void repl_line_cb(int fd, xEventMask mask, void *arg) {
  (void)fd;
  (void)mask;
  ReplCtx *ctx = static_cast<ReplCtx *>(arg);
  if (!ctx->line) return;

  for (;;) {
    xLineStepResult r = xLineStep(ctx->line);
    switch (r) {
    case XLINE_STEP_PENDING:
      return;
    case XLINE_STEP_LINE: {
      char *s = xLineTake(ctx->line);
      /* Close the finished editor session and *immediately* reopen a
       * fresh one before dispatching. That way slash-command output
       * (which goes through above_printf) has a live handle to draw
       * onto, and AI submission hands control back to the event loop
       * with the new session already active to receive on_text /
       * on_done Above calls. */
      repl_close_line(ctx);
      if (repl_open_line(ctx) != 0) {
        xLineFree(s);
        xEventLoopStop(ctx->loop);
        return;
      }
      int want_stop = repl_handle_line(ctx, s);
      xLineFree(s);
      if (want_stop) {
        xEventLoopStop(ctx->loop);
        return;
      }
      return;
    }
    case XLINE_STEP_INTERRUPT: {
      /* User hit Ctrl-C. xline disables ISIG in raw mode, so ^C
       * never becomes a real SIGINT — it surfaces here instead.
       *
       * Two distinct semantics depending on AI state:
       *   busy → abort the in-flight run, keep the REPL alive
       *          (mirrors /cancel exactly: xAgentSessionCancel is
       *          async, on_done will eventually arrive with
       *          reason=Aborted and flip ctx->busy back off).
       *   idle → treat as a request to leave the REPL. A second
       *          ^C on an empty prompt is the conventional exit.
       *
       * Either way the old editor session has already been
       * finalised by xline (buffer wiped, state = DONE_INTERRUPT),
       * so we must close + reopen it to get a fresh prompt drawn
       * with the cursor back at column 0. */
      repl_close_line(ctx);
      if (ctx->busy) {
        if (repl_open_line(ctx) != 0) {
          xEventLoopStop(ctx->loop);
          return;
        }
        above_printf(ctx->line, "\x1b[2m[cancel] aborting run…\x1b[0m");
        xAgentSessionCancel(ctx->sess);
        return;
      }
      xEventLoopStop(ctx->loop);
      return;
    }
    case XLINE_STEP_EOF:
    case XLINE_STEP_ERROR:
      /* Ctrl-D on empty input, or a fatal tty error. Treat either
       * as a clean shutdown. */
      repl_close_line(ctx);
      xEventLoopStop(ctx->loop);
      return;
    }
  }
}

static void repl_on_sigint(int signo, void *arg) {
  /* Kept as a no-op stub only so grep/history still maps ^C to a
   * single place. In practice we never get here: xline puts the
   * tty in raw mode with ISIG cleared, so Ctrl-C arrives as a
   * byte (0x03) that xLineStep surfaces as XLINE_STEP_INTERRUPT.
   * The real cancel/exit logic lives in repl_on_readable's
   * INTERRUPT arm. This watch is only retained for the rare
   * paths that can still deliver SIGINT (kill -INT, GUI terminal
   * sending SIGINT explicitly): redirect them to the same
   * behaviour as the in-band interrupt. */
  (void)signo;
  ReplCtx *ctx = static_cast<ReplCtx *>(arg);
  if (ctx->busy) {
    xAgentSessionCancel(ctx->sess);
    return;
  }
  xLineAsyncStop();
}

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
  xAgentOpenAIConf pconf;
  std::memset(&pconf, 0, sizeof(pconf));
  pconf.api_key       = api_key;
  pconf.base_url      = api_url;
  pconf.default_model = model;
  pconf.timeout_ms    = 60000;

  xAgentProvider pvd = xAgentProviderOpenAICreate(loop, http, &pconf);
  if (!pvd) {
    std::fprintf(stderr, "failed to create OpenAI provider\n");
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Tools ─────────────────────────────────────────────────────────── */

  /* Declare the REPL context up front so the shell tool's callbacks
   * can capture a pointer to it. Fields are populated further below
   * as the objects they reference come into existence. */
  ReplCtx ctx;
  ctx.loop = loop;

  xAgentShellConf shell_conf;
  std::memset(&shell_conf, 0, sizeof(shell_conf));
  shell_conf.callback_ud = &ctx;
  shell_conf.on_command  = [](const char *command, const char *cwd, void *ud) {
    auto *c = static_cast<ReplCtx *>(ud);
    if (cwd && cwd[0]) {
      above_printf(c->line, "\x1b[2m  $ (cd %s && %s)\x1b[0m", cwd, command);
    } else {
      above_printf(c->line, "\x1b[2m  $ %s\x1b[0m", command);
    }
  };
  shell_conf.on_result = [](int exit_code, size_t stdout_len, size_t stderr_len,
                            int timed_out, void *ud) {
    auto *c = static_cast<ReplCtx *>(ud);
    above_printf(c->line, "\x1b[2m  exit=%d stdout=%zu stderr=%zu%s\x1b[0m",
                 exit_code, stdout_len, stderr_len,
                 timed_out ? " (timed out)" : "");
  };
  xAgentTool shell_tool = xAgentToolShellCreate(loop, &shell_conf);
  if (!shell_tool) {
    std::fprintf(stderr, "failed to create shell tool\n");
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  const xAgentTool *tool_ptrs[] = {&shell_tool};
  const size_t   TOTAL_TOOLS = 1;
  /* ── Session config (agent's default session) ──────────────────────
   *
   * Instead of creating a session manually and managing its
   * lifecycle, we set default_session_conf on the agent so it
   * creates a built-in default session at construction time.
   * The session is retrieved via xAgentDefaultSession() and
   * is destroyed automatically by xAgentDestroy(). */

  xAgentSessionConf sconf;
  std::memset(&sconf, 0, sizeof(sconf));
  sconf.cbs.on_text        = on_text;
  sconf.cbs.on_thinking    = on_thinking;
  sconf.cbs.on_tool        = on_tool;
  sconf.cbs.on_tool_output = on_tool_output;
  sconf.cbs.on_sidecar     = on_sidecar;
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
  sconf.budget.policy            = xAgentBudgetPolicy_Auto;
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
  xAgentConf aconf;
  std::memset(&aconf, 0, sizeof(aconf));
  aconf.loop     = loop;
  aconf.provider = pvd;
  aconf.model    = model;
  aconf.system_prompt =
    "You are a concise assistant running on xKit's xagent session "
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

  xAgent agent = xAgentCreate(&aconf);
  if (!agent) {
    std::fprintf(stderr, "failed to create agent\n");
    xAgentToolDestroy(shell_tool);
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* Retrieve the agent's built-in default session — no manual
   * create/destroy needed. The session lives for the agent's
   * entire lifetime. */
  xAgentSession sess = xAgentDefaultSession(agent);
  if (!sess) {
    std::fprintf(stderr, "agent has no default session\n");
    xAgentDestroy(agent);
    xAgentToolDestroy(shell_tool);
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  ctx.sess = sess;

  /* ── Startup banner ───────────────────────────────────────────────
   *
   * Printed once in cooked mode before repl_open_line paints the
   * prompt. A 60-col bordered box: the top bar carries the demo
   * title, the body lists the knobs that actually vary between runs
   * (model, tool list, data_dir) and a Tips block with the keys /
   * commands a first-time user needs. Everything else lives behind
   * /help so the banner doesn't grow with every new command.
   *
   * Styling: ANSI bold for the title, faint for the border. Box
   * drawing uses Unicode (any modern terminal; collapses visually
   * on a dumb tty but still prints sensibly).
   *
   * Width discipline: inside the box we rely on the fact that every
   * body line is pure ASCII, so byte count == display width. That
   * lets printf's %-Ns pad to the right │ without manual counting.
   * `model` and `data_dir` are user-supplied so we truncate them to
   * fit the inner 56-col budget instead of blowing the frame. */
  enum {
    BOX_INNER = 56
  }; // visible cols between "│ " and " │"
  char line[BOX_INNER + 1];
  // top: "┌─ AI Agent Core Demo " is 22 cells; + 37 '─' + '┐' = 60
  std::printf("\x1b[2m┌─ \x1b[22m\x1b[1mAI Agent Core Demo\x1b[22m"
              "\x1b[2m ─────────────────────────────────────┐\x1b[22m\n");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  std::snprintf(line, sizeof(line), "model=%s, tools=shell", model);
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, line);
  std::snprintf(line, sizeof(line), "data_dir: %s", data_dir);
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, line);
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "Tips:");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  - Enter       send message");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  - '/' + Tab   browse slash commands");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  - /help       show all commands");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  - /cancel     interrupt a running AI call");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  - Ctrl-C      cancel current run / exit when idle");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  - Ctrl-D      exit on empty line");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  // bottom: '└' + 58 '─' + '┘' = 60
  std::printf("\x1b[2m└───────────────────────────────────────────────────"
              "───────┘\x1b[22m\n\n");

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

  /* Let the edit region flow right after the above-region stream
   * instead of being pinned to the bottom of the terminal. Matches
   * classic readline layout, which is what this demo's transcript-
   * style UX expects. */
  xLineEnableAnchor(false);

  /* Flush stdout before handing the terminal to xline. Any pending
   * output (the banner above) must clear cooked mode or it can
   * interleave with the prompt paint below. */
  std::fflush(stdout);

  if (repl_open_line(&ctx) != 0) {
    xAgentDestroy(agent);
    for (size_t i = 0; i < TOTAL_TOOLS; ++i)
      xAgentToolDestroy(*tool_ptrs[i]);
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  xEventLoopSignalWatch(loop, SIGINT, repl_on_sigint, &ctx);

  xEventLoopRun(loop);

  xEventLoopSignalWatch(loop, SIGINT, nullptr, nullptr);
  repl_close_line(&ctx);

  std::printf("\nBye!\n");

  /* No xAgentSessionDestroy needed — the default session is owned
   * by the agent and destroyed automatically in xAgentDestroy. */
  xAgentDestroy(agent);
  for (size_t i = 0; i < TOTAL_TOOLS; ++i)
    xAgentToolDestroy(*tool_ptrs[i]);
  xAgentProviderDestroy(pvd);
  xHttpClientDestroy(http);
  xEventLoopDestroy(loop);
  return 0;
}
