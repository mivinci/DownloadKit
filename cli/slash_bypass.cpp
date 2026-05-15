/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash_bypass.cpp - /bypass slash command: skip tool confirms.
 */

#include "slash_cmd.h"

#include "output.h"

#include <cstdio>
#include <cstring>

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

void slash_cmd_bypass(ReplCtx *ctx, const char *args) {
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
void slash_argc_bypass(xLineCompletionEnv cenv, ReplCtx *ctx,
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
