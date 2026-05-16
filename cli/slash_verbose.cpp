/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash_verbose.cpp - /verbose slash command: toggle tool output verbosity.
 */

#include "slash_cmd.h"

#include "output.h"

#include <cstdio>
#include <cstring>

/* ── /verbose [on|off]: toggle tool output verbosity ─────────────
 *
 * When OFF, on_tool_output short-circuits so the data stream is
 * suppressed while [tool] start/finish, command echo ($ <cmd>),
 * and result summary (exit=...) all still render.
 *
 * Shape:
 *   /verbose       → print current state + usage hint
 *   /verbose on    → enable full output
 *   /verbose off   → suppress tool data output
 *
 * Flag is per-process: a fresh cli invocation always starts with
 * verbose disabled. */
void slash_cmd_verbose(ReplCtx *ctx, const char *args) {
  /* No args: report current state + usage. Shown in the below panel
   * so it stays pinned (symmetry with /model, /bypass, /renderer). */
  if (!args || !*args) {
    const char *state = ctx->verbose_tool_output ? "on" : "off";
    char        body[512];
    std::snprintf(body, sizeof(body),
                  "current: %s\n\n"
                  "usage:\n"
                  "  /verbose on     show full tool output\n"
                  "  /verbose off    suppress tool data (summary only)\n"
                  "\n"
                  "when OFF, the raw stdout/stderr of tool calls is\n"
                  "hidden; only the command, exit code, and byte totals\n"
                  "are shown.",
                  state);
    xLineSetBelowPanel(ctx->line, "verbose", body);
    return;
  }

  if (std::strcmp(args, "on") == 0) {
    if (ctx->verbose_tool_output) {
      above_printf(ctx->line, "\x1b[2m[verbose] already on\x1b[0m");
      return;
    }
    ctx->verbose_tool_output = true;
    above_printf(ctx->line, "\x1b[2m[verbose] on \u2014 full tool output shown\x1b[0m");
    return;
  }

  if (std::strcmp(args, "off") == 0) {
    if (!ctx->verbose_tool_output) {
      above_printf(ctx->line, "\x1b[2m[verbose] already off\x1b[0m");
      return;
    }
    ctx->verbose_tool_output = false;
    above_printf(ctx->line, "\x1b[2m[verbose] off \u2014 tool data suppressed"
                            " (summary only)\x1b[0m");
    return;
  }

  /* Fall-through: unknown tail. */
  above_printf(ctx->line, "\x1b[2m[verbose] unrecognised args: %s\x1b[0m", args);
  above_printf(ctx->line, "\x1b[2m         usage: /verbose [on | off]\x1b[0m");
}

/* Arg completer for /verbose. Offers "on" and "off". */
void slash_argc_verbose(xLineCompletionEnv cenv, ReplCtx *ctx, const char *token) {
  (void)ctx;
  static const struct {
    const char *name;
    const char *help;
  } cands[] = {
    {"on", "show full tool output"},
    {"off", "suppress tool data output (show summary only)"},
  };
  for (const auto &c : cands) {
    if (!xLineStartsWith(c.name, token)) continue;
    xLineAddCompletionEx(cenv, c.name, c.name, c.help);
  }
}
