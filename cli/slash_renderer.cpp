/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash_renderer.cpp - /renderer slash command: switch output renderer.
 */

#include "slash_cmd.h"

#include "output.h"

#include <cstdio>
#include <cstring>

/* ── /renderer [md|raw]: switch output rendering backend ────────────
 *
 * Takes effect on the NEXT streamed chunk. If a run is in flight the
 * current turn's output was already being rendered through whatever
 * backend was active; the switch only changes the routing for chunks
 * that arrive after this command. Flush the old backend first so any
 * pending state (e.g. half-parsed emphasis) is emitted before we
 * swap the vtable. */
void slash_cmd_renderer(ReplCtx *ctx, const char *args) {
  /* Trim leading whitespace. */
  while (*args == ' ' || *args == '\t') args++;

  /* No argument: show current mode + usage, matching /model's layout. */
  if (!*args) {
    const char *mode = ctx->renderer_name ? ctx->renderer_name : "md";
    char body[256];
    std::snprintf(body, sizeof(body),
                  "current: %s\n\n"
                  "usage:\n"
                  "  /renderer md    markdown → ANSI rendering\n"
                  "  /renderer raw   verbatim (no formatting)",
                  mode);
    xLineSetBelowPanel(ctx->line, "renderer", body);
    return;
  }

  if (std::strcmp(args, "md") == 0) {
    ctx->renderer.flush(ctx->renderer.state);
    renderer_use_md(ctx);
    above_printf(ctx->line, "\x1b[2m[render] switched to markdown\x1b[0m");
  } else if (std::strcmp(args, "raw") == 0) {
    ctx->renderer.flush(ctx->renderer.state);
    renderer_use_raw(ctx);
    above_printf(ctx->line, "\x1b[2m[render] switched to raw\x1b[0m");
  } else {
    above_printf(ctx->line,
                 "\x1b[2m[render] unknown mode \"%s\" — use /renderer md "
                 "or /renderer raw\x1b[0m",
                 args);
  }
}

void slash_argc_renderer(xLineCompletionEnv cenv, ReplCtx *ctx,
                         const char *token) {
  (void)ctx;
  static const struct {
    const char *name;
    const char *help;
  } cands[] = {
    {"md",  "markdown → ANSI rendering"},
    {"raw", "verbatim (no formatting)"},
  };
  for (const auto &c : cands) {
    if (!xLineStartsWith(c.name, token)) continue;
    xLineAddCompletionEx(cenv, c.name, c.name, c.help);
  }
}
