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

#include "ctx.h"
#include "output.h"
#include "repl.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <xagent/model.h>
#include <xagent/session.h>
#include <xbase/time.h>
#include <xline/line.h>

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
typedef void (*SlashArgCompleter)(xLineCompletionEnv cenv, ReplCtx *ctx,
                                  const char *token);

struct SlashCmd {
  const char       *name; /* including leading '/' */
  const char       *help;
  SlashCmdFunc      fn;
  SlashArgCompleter arg_completer; /* NULL = no arg completion */
};

static void slash_cmd_help(ReplCtx *ctx, const char *args);
static void slash_cmd_exit(ReplCtx *ctx, const char *args);
static void slash_cmd_clear(ReplCtx *ctx, const char *args);
static void slash_cmd_history(ReplCtx *ctx, const char *args);
static void slash_cmd_tokens(ReplCtx *ctx, const char *args);
static void slash_cmd_cancel(ReplCtx *ctx, const char *args);
static void slash_cmd_model(ReplCtx *ctx, const char *args);
static void slash_cmd_version(ReplCtx *ctx, const char *args);
static void slash_argc_model(xLineCompletionEnv cenv, ReplCtx *ctx,
                             const char *token);
static void slash_cmd_bypass(ReplCtx *ctx, const char *args);
static void slash_argc_bypass(xLineCompletionEnv cenv, ReplCtx *ctx,
                              const char *token);

static const SlashCmd g_slash_cmds[] = {
  {"/help", "show this help", slash_cmd_help, nullptr},
  {"/exit", "quit the REPL", slash_cmd_exit, nullptr},
  {"/clear", "clear the terminal screen", slash_cmd_clear, nullptr},
  {"/history", "print input history", slash_cmd_history, nullptr},
  {"/tokens", "show cumulative token usage", slash_cmd_tokens, nullptr},
  {"/cancel", "interrupt the active AI run", slash_cmd_cancel, nullptr},
  {"/version", "show the xKit build version", slash_cmd_version, nullptr},
  {"/model", "show / switch the active model (e.g. /model kimi)",
   slash_cmd_model, slash_argc_model},
  {"/bypass",
   "skip tool confirms (/bypass on --yes | off)",
   slash_cmd_bypass, slash_argc_bypass},
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
static const SlashCmd *slash_match_cmd_for_arg(const char *prefix,
                                               size_t     *arg_start_out) {
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
    if (std::strlen(c->name) == cmd_len &&
        std::strncmp(c->name, prefix, cmd_len) == 0) {
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
static thread_local ReplCtx       *tls_arg_ctx;

static void slash_arg_completer_inner(xLineCompletionEnv cenv,
                                      const char        *token) {
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
  /* Enumerate the in-memory history via xLineHistoryGet. We used to
   * read the persisted .ai_session_history file directly, but that
   * file stores non-ASCII bytes using isocline's internal \xHH
   * escape format (see history_write_entry in modules/xline/
   * history.c) — CJK and emoji came out as a wall of "\xE7\x9C\x8B"
   * literals. The in-memory buffer is already decoded, which is
   * exactly what the user wants to see. As a bonus this also picks
   * up entries from the current session that haven't been flushed
   * to disk yet. */
  long n = xLineHistoryCount();
  if (n <= 0) {
    xLineSetBelowPanel(
      ctx->line, nullptr,
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

/* /version — dump the CMake-injected XKIT_VERSION so users can ask
 * the REPL "what am I running" without reading the banner. Uses the
 * same BelowPanel channel as /tokens for consistency. */
static void slash_cmd_version(ReplCtx *ctx, const char *args) {
  (void)args;
  xLineSetBelowPanel(ctx->line, nullptr, "xKit " XKIT_VERSION);
}

/* Render the registry as a "id -> wire model" listing. Used by
 * /model (when no argument is given) so the user can see every
 * registered id alongside the "*" marker for the active one. */
static std::string render_registry_listing(const ReplCtx *ctx) {
  std::string body;
  if (!ctx->model_registry) return "(no registry \u2014 startup was misconfigured)";

  size_t n = xAgentModelRegistryCount(ctx->model_registry);
  for (size_t i = 0; i < n; ++i) {
    const xAgentModelSpec *s = xAgentModelRegistryAt(ctx->model_registry, i);
    if (!s) continue;
    char line[256];
    /* Mark the currently-active spec with "*" so the user can tell
     * which one /model with no args would re-select. */
    const char *mark =
      (ctx->current_model_id == s->id) ? "*" : " ";
    std::snprintf(line, sizeof(line), " %s %-12s %s", mark, s->id,
                  s->model ? s->model : "(provider default)");
    if (!body.empty()) body.push_back('\n');
    body.append(line);
  }
  if (body.empty()) body = "(no models registered)";
  return body;
}

/* Arg completer for /model: enumerate every id in the registry whose
 * name extends the typed token. Display uses the id; help shows the
 * wire model so the menu doubles as a "what does this id map to?"
 * reference. Startup misconfiguration (no registry) simply yields
 * no completions — the hint stays quiet instead of crashing. */
static void slash_argc_model(xLineCompletionEnv cenv, ReplCtx *ctx,
                             const char *token) {
  if (!ctx || !ctx->model_registry) return;
  size_t n = xAgentModelRegistryCount(ctx->model_registry);
  for (size_t i = 0; i < n; ++i) {
    const xAgentModelSpec *s = xAgentModelRegistryAt(ctx->model_registry, i);
    if (!s || !s->id) continue;
    if (!xLineStartsWith(s->id, token)) continue;
    const char *wire = s->model ? s->model : "(provider default)";
    xLineAddCompletionEx(cenv, s->id, s->id, wire);
  }
}

static void slash_cmd_model(ReplCtx *ctx, const char *args) {
  /* No argument: show the current selection + full listing in the
   * below panel. This doubles as "list every registered id" so
   * there's only one command to remember. */
  if (!args || !*args) {
    std::string body = "current: " + ctx->current_model_id + "\n\n";
    body += render_registry_listing(ctx);
    xLineSetBelowPanel(ctx->line, "model", body.c_str());
    return;
  }

  /* Reject the switch while a run is in flight — xAgentSessionSetModel
   * only affects the NEXT Query, but changing mid-run is still
   * surprising and the REPL's busy-flag bookkeeping assumes a
   * single provider per in-flight Query. Ask the user to /cancel
   * first. */
  if (ctx->busy) {
    above_printf(ctx->line,
                 "\x1b[2m(cannot switch model while a run is in flight; "
                 "try /cancel first)\x1b[0m");
    return;
  }

  /* Degraded "no model configured" mode: without a session there's
   * nothing to switch. We still rendered the (empty) listing branch
   * above so /model with no args keeps working as a discovery tool;
   * only the switch path bails out here. */
  if (!ctx->sess) {
    above_printf(ctx->line,
                 "\x1b[1;33m[no model]\x1b[22;39m cannot switch \u2014 "
                 "edit models.json in your data_dir and restart.");
    return;
  }

  xErrno rc = xAgentSessionSetModel(ctx->sess, args);
  if (rc == xErrno_NotFound) {
    above_printf(ctx->line,
                 "unknown model id: %s  (try /model to see available ids)",
                 args);
    return;
  }
  if (rc != xErrno_Ok) {
    above_printf(ctx->line, "/model: failed (err=%d)", (int)rc);
    return;
  }

  /* Success — record the new selection so the listing's "*" marker
   * and future banner updates reflect it. */
  ctx->current_model_id = args;

  const xAgentModelSpec *spec =
    xAgentModelRegistryGet(ctx->model_registry, args);
  const char *wire = spec && spec->model ? spec->model : "(provider default)";
  above_printf(ctx->line,
               "\x1b[2m[model] switched to id=%s (wire model=%s)\x1b[0m",
               args, wire);
}

/* ── /bypass: skip all tool confirms for the rest of the session ──
 *
 * High-risk feature (arbitrary shell with no approval gate), so the
 * ON path takes explicit parameters rather than acting as a plain
 * toggle. Shape:
 *
 *   /bypass               → print current state + usage hint
 *   /bypass on            → refuse, ask for --yes
 *   /bypass on --yes      → enable
 *   /bypass off           → disable (no confirmation needed; turning
 *                           safety back on is always fine)
 *
 * Flag is per-process and per-ReplCtx: a fresh cli invocation
 * always starts with confirms enabled. This is deliberate — auto-
 * persisting the flag across runs would let a forgotten enable
 * bleed into unrelated future sessions.
 *
 * Unrecognised tails fall through to the usage hint so typos like
 * `/bypass yes` don't silently enable or silently do nothing. */
static bool bypass_args_are(const char *args, const char *want) {
  /* Case-sensitive whole-token match with arbitrary trailing
   * whitespace trimmed. Keeps `on`/`off`/`on --yes` recognition
   * simple without dragging in a full arg splitter. */
  if (!args || !want) return false;
  size_t wlen = std::strlen(want);
  if (std::strncmp(args, want, wlen) != 0) return false;
  const char *tail = args + wlen;
  while (*tail == ' ' || *tail == '\t')
    ++tail;
  return *tail == '\0';
}

static bool bypass_has_yes(const char *args) {
  /* Scan the tail for a `--yes` token surrounded by whitespace or
   * string boundaries. Deliberately conservative: `--yesterday`
   * will NOT match, so a future `--yesterday` flag (if we ever add
   * one) won't accidentally unlock bypass. */
  if (!args) return false;
  const char *p = args;
  while ((p = std::strstr(p, "--yes")) != nullptr) {
    bool        left_ok  = (p == args) || p[-1] == ' ' || p[-1] == '\t';
    const char *after    = p + 5; /* strlen("--yes") */
    bool        right_ok = *after == '\0' || *after == ' ' || *after == '\t';
    if (left_ok && right_ok) return true;
    p = after;
  }
  return false;
}

static void slash_cmd_bypass(ReplCtx *ctx, const char *args) {
  /* No args: report current state + usage. Shown in the below panel
   * so it stays pinned until the user does something else (symmetry
   * with /model and /help). */
  if (!args || !*args) {
    const char *state = ctx->bypass_confirm ? "ON" : "off";
    char        body[512];
    std::snprintf(body, sizeof(body),
                  "current: %s\n\n"
                  "usage:\n"
                  "  /bypass on --yes   enable (skip ALL tool confirms)\n"
                  "  /bypass off        disable (restore confirm prompts)\n"
                  "\n"
                  "when ON, every tool that would normally ask for\n"
                  "approval (shell, etc) runs immediately. use only\n"
                  "when you trust the current task.",
                  state);
    xLineSetBelowPanel(ctx->line, "bypass", body);
    return;
  }

  if (bypass_args_are(args, "off")) {
    if (!ctx->bypass_confirm) {
      above_printf(ctx->line, "\x1b[2m[bypass] already off\x1b[0m");
      return;
    }
    ctx->bypass_confirm = false;
    above_printf(ctx->line,
                 "\x1b[2m[bypass] disabled \u2014 tool confirms restored"
                 "\x1b[0m");
    return;
  }

  if (bypass_args_are(args, "on")) {
    /* Bare `/bypass on` — refuse + show the gate. Yellow (not red)
     * because this is guidance ("here's what you should type
     * instead"), not a live danger state. Red is reserved for the
     * ENABLED-and-persisting case below, so the two don't visually
     * collapse into one wall of red. Matches the palette of the
     * `confirm>` prompt. */
    above_printf(
      ctx->line,
      "\x1b[33m[bypass] refusing: this disables ALL tool confirms."
      "\x1b[0m");
    above_printf(ctx->line,
                 "\x1b[33m         to proceed, run: /bypass on --yes"
                 "\x1b[0m");
    return;
  }

  /* `on --yes` (in either order, tolerant of extra whitespace). */
  if (bypass_has_yes(args)) {
    /* Require `on` to be present somewhere too — `/bypass --yes`
     * alone is ambiguous (enable? disable? check?) and we'd rather
     * make the intent explicit. */
    const char *on_pos = std::strstr(args, "on");
    bool        on_ok  = false;
    if (on_pos) {
      bool        left_ok  = (on_pos == args) || on_pos[-1] == ' ' ||
                             on_pos[-1] == '\t';
      const char *after    = on_pos + 2;
      bool        right_ok = *after == '\0' || *after == ' ' || *after == '\t';
      on_ok                = left_ok && right_ok;
    }
    if (on_ok) {
      if (ctx->bypass_confirm) {
        above_printf(ctx->line, "\x1b[2m[bypass] already on\x1b[0m");
        return;
      }
      ctx->bypass_confirm = true;
      above_printf(ctx->line,
                   "\x1b[1;31m[bypass] ENABLED \u2014 tool confirms are now"
                   " skipped for this session.\x1b[0m");
      above_printf(ctx->line,
                   "\x1b[1;31m         run /bypass off to restore.\x1b[0m");
      return;
    }
  }

  /* Fall-through: unknown tail. Don't guess the intent — surface
   * the usage hint and leave state untouched. */
  above_printf(ctx->line,
               "\x1b[2m[bypass] unrecognised args: %s\x1b[0m", args);
  above_printf(ctx->line,
               "\x1b[2m         usage: /bypass [on --yes | off]\x1b[0m");
}

/* Arg completer for /bypass. Candidates depend on what's been typed
 * so far: an empty token (or partial of `on`/`off`) gets the verbs;
 * if `on` is already there, the `--yes` flag is offered. Keeps the
 * menu minimal so the user isn't wading through noise. */
static void slash_argc_bypass(xLineCompletionEnv cenv, ReplCtx *ctx,
                              const char *token) {
  (void)ctx;
  /* xLineCompleteWord extracts only the word at the cursor, so here
   * `token` is the CURRENT partial word, not the whole arg string.
   * We can't see earlier words (e.g. we don't know if `on` has
   * already been typed). Simplest correct behaviour: always offer
   * the three primary tokens and let xline's prefix filter sort it
   * out. `--yes` won't appear unless the user has started typing
   * `-`, which is the natural moment to suggest it. */
  static const struct {
    const char *name;
    const char *help;
  } cands[] = {
    {"on",    "enable bypass (requires --yes)"},
    {"off",   "disable bypass"},
    {"--yes", "confirm enabling bypass"},
  };
  for (const auto &c : cands) {
    if (!xLineStartsWith(c.name, token)) continue;
    xLineAddCompletionEx(cenv, c.name, c.name, c.help);
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
