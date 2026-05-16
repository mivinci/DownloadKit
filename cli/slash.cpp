/*
 * Copyright 2025 The moo Authors. All rights reserved.
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
#include "slash_cmd.h"

#include "ctx.h"
#include "output.h"
#include "repl.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <x/agent/session.h>
#include <x/base/time.h>
#include <x/line/line.h>

typedef void (*SlashCmdFunc)(ReplCtx *ctx, const char *args);

/* Per-command argument completer: invoked when the user is typing the
 * first positional argument of `/<cmd>` (i.e. the input already
 * matches `/<cmd><space>+<token>` up to the cursor). `token` is the
 * partial argument extracted by xLineCompleteWord; handlers should
 * feed replacements via xLineAddCompletionEx, which will correctly
 * delete-before the partial. Return with no completions added to
 * produce a silent no-op (keeps the hint overlay quiet).
 *
 * This is the "xLineSetArgCompleter(\"/foo\", ...)" equivalent: it's a
 * table lookup rather than a separate API, but the ergonomics are
 * the same — one function per command, no shared global state. */
typedef void (*SlashArgCompleter)(xLineCompletionEnv cenv, ReplCtx *ctx, const char *token);

struct SlashCmd {
  const char       *name; /* including leading '/' */
  const char       *help;
  SlashCmdFunc      fn;
  SlashArgCompleter arg_completer; /* NULL = no arg completion */
};

/* Forward declarations for handlers defined in this file. */
static void slash_cmd_help(ReplCtx *ctx, const char *args);
static void slash_cmd_exit(ReplCtx *ctx, const char *args);
static void slash_cmd_clear(ReplCtx *ctx, const char *args);
static void slash_cmd_history(ReplCtx *ctx, const char *args);
static void slash_cmd_tokens(ReplCtx *ctx, const char *args);
static void slash_cmd_cancel(ReplCtx *ctx, const char *args);
static void slash_cmd_version(ReplCtx *ctx, const char *args);

static const SlashCmd g_slash_cmds[] = {
  {"/help", "show this help", slash_cmd_help, nullptr},
  {"/exit", "quit the REPL", slash_cmd_exit, nullptr},
  {"/clear", "clear the terminal screen", slash_cmd_clear, nullptr},
  {"/history", "print input history", slash_cmd_history, nullptr},
  {"/tokens", "show cumulative token usage", slash_cmd_tokens, nullptr},
  {"/cancel", "interrupt the active AI run", slash_cmd_cancel, nullptr},
  {"/version", "show the moo build version", slash_cmd_version, nullptr},
  {"/model", "show / switch the active model (e.g. /model kimi)", slash_cmd_model,
   slash_argc_model},
  {"/bypass", "skip tool confirms (/bypass on --yes | off)", slash_cmd_bypass, slash_argc_bypass},
  {"/renderer", "set output renderer (md | raw)", slash_cmd_renderer, slash_argc_renderer},
  {"/verbose", "toggle tool output verbosity (/verbose on | off)", slash_cmd_verbose,
   slash_argc_verbose},
};
static const size_t g_slash_cmds_count = sizeof(g_slash_cmds) / sizeof(g_slash_cmds[0]);

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
  return c == '/' || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9');
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

/* Find the SlashCmd whose name is the first whitespace-delimited
 * token of `prefix` (the input up to the cursor) AND is immediately
 * followed by at least one space. Returns NULL when the cursor is
 * still inside the command token itself — that's the signal to fall
 * through to command-name completion instead of argument completion.
 *
 * `*arg_start_out` (optional) receives the offset into `prefix` of
 * the first character of the argument region (first non-space after
 * the command). Used only for sanity checks; the actual token
 * extraction is handled by xLineCompleteWord downstream. */
static const SlashCmd *slash_match_cmd_for_arg(const char *prefix, size_t *arg_start_out) {
  if (!prefix || prefix[0] != '/') return nullptr;
  /* Walk to the end of the command token. */
  size_t i = 0;
  while (prefix[i] && prefix[i] != ' ' && prefix[i] != '\t')
    ++i;
  /* Need at least one whitespace after the command to be "in args". */
  if (prefix[i] != ' ' && prefix[i] != '\t') return nullptr;
  size_t cmd_len = i;
  /* Skip the separator run. */
  while (prefix[i] == ' ' || prefix[i] == '\t')
    ++i;
  if (arg_start_out) *arg_start_out = i;
  for (size_t k = 0; k < g_slash_cmds_count; ++k) {
    const SlashCmd *c = &g_slash_cmds[k];
    if (std::strlen(c->name) == cmd_len && std::strncmp(c->name, prefix, cmd_len) == 0) {
      return c;
    }
  }
  return nullptr;
}

/* Trampoline: xLineCompleteWord hands us the argument token with the
 * delete-before bookkeeping already primed. We forward to the
 * command-specific arg completer found in the SlashCmd table. The
 * pair (cmd, ctx) is smuggled through a thread-local since
 * xLineCompleteWord's signature doesn't carry user closure. This is
 * fine: completion is always single-threaded inside xline's event
 * loop and the variable is written then read inside the same call. */
static thread_local const SlashCmd *tls_arg_cmd;
static thread_local ReplCtx        *tls_arg_ctx;

static void slash_arg_completer_inner(xLineCompletionEnv cenv, const char *token) {
  const SlashCmd *c   = tls_arg_cmd;
  ReplCtx        *ctx = tls_arg_ctx;
  if (c && c->arg_completer) c->arg_completer(cenv, ctx, token);
}

/* Top-level completer registered with xline. Two modes:
 *   1) cursor still inside the command token (`/cl<TAB>`) — run the
 *      command-name completer, identical to the original behaviour.
 *   2) cursor past `/<cmd> ` — look up that command's arg completer
 *      and run it through xLineCompleteWord so the partial argument
 *      gets replaced cleanly (no `/model k/model kimi` artifacts).
 *
 * We delegate to xLineCompleteWord in both modes so prefix extraction
 * and replacement semantics stay consistent with the rest of xline. */
static void slash_completer(xLineCompletionEnv cenv, const char *prefix) {
  const SlashCmd *cmd = slash_match_cmd_for_arg(prefix, nullptr);
  if (cmd && cmd->arg_completer) {
    /* Argument-completion mode. Using the default word-char predicate
     * (NULL → xLineCharIsNonseparator) means any non-whitespace run
     * is treated as the arg token, which matches how users type
     * model ids, filenames, etc. */
    tls_arg_cmd = cmd;
    tls_arg_ctx = static_cast<ReplCtx *>(xLineCompletionArg(cenv));
    xLineCompleteWord(cenv, prefix, slash_arg_completer_inner, nullptr);
    tls_arg_cmd = nullptr;
    tls_arg_ctx = nullptr;
    return;
  }
  /* Command-name mode (original path). */
  (void)prefix; /* xLineCompleteWord re-derives it using our predicate */
  xLineCompleteWord(cenv, prefix, slash_completer_inner, is_slash_cmd_char);
}

void slash_install_completer(ReplCtx *ctx) {
  xLineSetDefaultCompleter(slash_completer, ctx);
}

/* ── Trivial command handlers (short enough to stay in this file) ── */

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
    std::snprintf(line, sizeof(line), "  %-10s %s", g_slash_cmds[i].name, g_slash_cmds[i].help);
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
  /* Enumerate the in-memory history via xLineHistoryGet. We used to
   * read the persisted .ai_session_history file directly, but that
   * file stores non-ASCII bytes using isocline's internal \xHH
   * escape format (see history_write_entry in libs/xline/
   * history.c) — CJK and emoji came out as a wall of "\xE7\x9C\x8B"
   * literals. The in-memory buffer is already decoded, which is
   * exactly what the user wants to see. As a bonus this also picks
   * up entries from the current session that haven't been flushed
   * to disk yet. */
  long n = xLineHistoryCount();
  if (n <= 0) {
    xLineSetBelowPanel(ctx->line, nullptr,
                       "(no history yet \u2014 submit a message and come back)");
    return;
  }
  std::string body;
  /* Walk oldest -> newest so the list reads top-to-bottom in the
   * order entries were submitted; xLineHistoryGet uses 0 = newest. */
  for (long i = n - 1; i >= 0; --i) {
    const char *entry = xLineHistoryGet(i);
    if (entry == nullptr) continue;
    if (!body.empty()) body.push_back('\n');
    body.append(entry);
  }
  if (body.empty()) body = "(empty)";
  char tail[64];
  std::snprintf(tail, sizeof(tail), "\n\n(%ld line(s); Ctrl-R to search)", n);
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
      std::snprintf(extra, sizeof(extra), " last_actual_prompt=%d", ctx->last_actual_prompt);
    }
    std::snprintf(buf, sizeof(buf), "\nbudget: remaining=%zu/%zu estimated=%zu%s",
                  ctx->budget_remaining, ctx->budget_limit, ctx->budget_estimated, extra);
    body.append(buf);
  } else {
    body.append("\nbudget: (no GatePassed event yet \u2014 submit a message first)");
  }
  xLineSetBelowPanel(ctx->line, nullptr, body.c_str());
}

/* /version — dump the CMake-injected MOO_VERSION so users can ask
 * the REPL "what am I running" without reading the banner. Uses the
 * same BelowPanel channel as /tokens for consistency. */
static void slash_cmd_version(ReplCtx *ctx, const char *args) {
  (void)args;
  xLineSetBelowPanel(ctx->line, nullptr, "moo " MOO_VERSION);
}

/* ── Dispatch ──────────────────────────────────────────────────────── */

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
    if (std::strlen(c->name) == cmd_len && std::strncmp(c->name, line, cmd_len) == 0) {
      if (c->fn) c->fn(ctx, args);
      return true;
    }
  }
  above_printf(ctx->line, "unknown command: %.*s  (try /help)", (int)cmd_len, line);
  return true;
}
