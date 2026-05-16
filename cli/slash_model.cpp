/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash_model.cpp - /model slash command: show / switch the active model.
 */

#include "slash_cmd.h"

#include "config.h"
#include "output.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <x/agent/model.h>
#include <x/agent/session.h>

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
    const char *mark = (ctx->current_model_id == s->id) ? "*" : " ";
    std::snprintf(line, sizeof(line), " %s %-12s %s", mark, s->id,
                  s->model ? s->model : "(provider default)");
    if (!body.empty()) body.push_back('\n');
    body.append(line);
  }
  if (body.empty()) body = "(no models registered)";
  return body;
}

void slash_argc_model(xLineCompletionEnv cenv, ReplCtx *ctx, const char *token) {
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

void slash_cmd_model(ReplCtx *ctx, const char *args) {
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
    above_printf(ctx->line, "\x1b[2m(cannot switch model while a run is in flight; "
                            "try /cancel first)\x1b[0m");
    return;
  }

  /* Degraded "no model configured" mode: without a session there's
   * nothing to switch. We still rendered the (empty) listing branch
   * above so /model with no args keeps working as a discovery tool;
   * only the switch path bails out here. */
  if (!ctx->sess) {
    above_printf(ctx->line, "\x1b[1;33m[no model]\x1b[22;39m cannot switch \u2014 "
                            "edit models.json in your data_dir and restart.");
    return;
  }

  xErrno rc = xAgentSessionSetModel(ctx->sess, args);
  if (rc == xErrno_NotFound) {
    above_printf(ctx->line, "unknown model id: %s  (try /model to see available ids)", args);
    return;
  }
  if (rc != xErrno_Ok) {
    above_printf(ctx->line, "/model: failed (err=%d)", (int)rc);
    return;
  }

  /* Success — record the new selection so the listing's "*" marker
   * and future banner updates reflect it. */
  ctx->current_model_id = args;

  /* Refresh the session's budget thresholds to match the newly-
   * selected model. cli_model_config_resolve_budget cascades
   * built-in defaults <- top-level "budget" <- the selected
   * entry's "budget", so any field this entry doesn't override
   * cleanly falls back through the layers. We then splice the
   * startup default back in for context_window if the cascade
   * left it at zero. The resulting conf is pushed into the
   * session via xAgentSessionSetBudget; policy and the event
   * callback pair are NOT touched (the session keeps the values
   * main.cpp installed at create time). */
  if (ctx->model_cfg) {
    xAgentBudgetConf b = cli_model_config_resolve_budget(ctx->model_cfg, args);
    if (b.context_window == 0) b.context_window = ctx->default_budget.context_window;
    xAgentSessionSetBudget(ctx->sess, &b);
  }

  const xAgentModelSpec *spec = xAgentModelRegistryGet(ctx->model_registry, args);
  const char            *wire = spec && spec->model ? spec->model : "(provider default)";
  above_printf(ctx->line, "\x1b[2m[model] switched to id=%s (wire model=%s)\x1b[0m", args, wire);
}
