/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash.cpp - Slash command table + dispatcher + Tab completer.
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
 * with a hypothetical future `/tokenize`.
 */

#include "slash.h"

#include "output.h"
#include "repl.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <xagent/session.h>
#include <xbase/time.h>
#include <xline/line.h>

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

void slash_install_completer(void) {
  xLineSetDefaultCompleter(slash_completer, nullptr);
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
  if (!ctx->busy && !ctx->confirm_active) {
    above_printf(ctx->line, "\x1b[2m(no AI run in flight)\x1b[0m");
    return;
  }
  /* xAgentSessionCancel is async: it asks the provider to unwind and
   * on_done is still delivered (with reason == Aborted). Flip busy
   * off there, not here — that keeps on_text/on_done ordering
   * consistent with the natural-completion path. If confirm mode is
   * active, drain the queue first so the session doesn't stall on a
   * pending resolver (Cancel would eventually turn them into no-ops,
   * but rejecting explicitly closes our own bookkeeping cleanly and
   * gets us back to a normal prompt immediately). */
  above_printf(ctx->line, "\x1b[2m[cancel] aborting run…\x1b[0m");
  if (ctx->confirm_active) {
    repl_drain_confirms_rejected(ctx, "cancelled by user");
  }
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
bool slash_dispatch(ReplCtx *ctx, const char *line) {
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
