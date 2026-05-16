/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * config.h - Host-app configuration: models.json parsing
 *
 * The CLI reads `<data_dir>/models.json` at startup and turns every
 * entry into a concrete xAgentProvider + a registry spec that pairs
 * it with the wire-level model name. The resulting xAgentModelRegistry
 * is handed to the agent so the host can switch between backends at
 * runtime via xAgentSessionSetModel(). See the module-level README
 * for the JSON schema; a minimal example:
 *
 * {
 *   "default": "kimi",
 *   "max_turns": 64,
 *   "budget": {
 *     "context_window": 8192
 *   },
 *   "models": [
 *     {"id": "kimi", "provider": "openai",
 *      "model": "kimi-k2.6",
 *      "api_key": "sk-...", "base_url": "https://api.moonshot.cn/v1",
 *      "budget": {"context_window": 131072}},
 *     {"id": "glm",  "provider": "openai",
 *      "model": "glm-4.5",
 *      "api_key": "sk-...", "base_url": "https://open.bigmodel.cn/api/paas/v4"}
 *   ]
 * }
 *
 * The top-level "max_turns" caps the tool-loop length per user input
 * (default: 64). The optional top-level "budget" block sets the
 * session's context-budget thresholds; every field is optional and
 * the missing ones fall back to xagent's built-in defaults. Any
 * field a per-model "budget" block also specifies overrides the
 * top-level value FOR THAT MODEL ONLY — the rest of the top-level
 * budget keeps applying. On /model switches the CLI re-merges
 * (top-level <- per-model) and pushes the result into the session
 * via xAgentSessionSetBudget so large and small models don't share
 * the same window.
 *
 * Everything is loaded once up front; the returned CliModelConfig
 * bundles both the raw provider handles (for later destruction) and
 * the registry that references them. See cli_model_config_destroy()
 * for the teardown order.
 */

#ifndef MOO_APPS_CLI_CONFIG_H
#define MOO_APPS_CLI_CONFIG_H

#include <string>
#include <vector>

#include <x/agent/model.h>
#include <x/agent/provider.h>
#include <x/agent/session.h>
#include <x/base/event.h>
#include <x/http/client.h>

/* Flags marking which budget fields were explicitly set by the
 * user. Needed because zero is a valid "use the built-in default"
 * sentinel for every threshold, so we cannot distinguish "missing"
 * from "explicitly 0" by inspecting the value alone — and per-model
 * overrides need to know which fields to actually override. */
struct CliBudgetMask {
  bool context_window = false;
};

/* User-supplied budget thresholds. Pairs with CliBudgetMask: only
 * fields whose mask bit is true should be consulted. Mirrors the
 * threshold subset of xAgentBudgetConf — policy and event callback
 * are NOT user-configurable (the CLI pins the policy to
 * Summarize and owns the event sink for its TUI overlay). */
struct CliBudgetConf {
  size_t        context_window = 0;
  CliBudgetMask mask;
};

/* One resolved entry: the concrete provider plus the metadata the
 * CLI wants to surface to the user. `model` is the wire name sent
 * to the provider — it's what /model prints and what the
 * banner shows for the current session. `budget` carries any
 * per-model overrides; only fields with a true mask bit win against
 * the top-level budget for this entry. */
struct CliModelEntry {
  std::string    id;        /* registry key, unique                        */
  std::string    provider_kind; /* "openai" today; future: "anthropic" etc.*/
  std::string    model;     /* wire name sent to the provider              */
  CliBudgetConf  budget;    /* per-model overrides; mask says which apply  */
  xAgentProvider provider;  /* owned; destroyed in cli_model_config_destroy*/
};

/* Fully-loaded model configuration. The registry borrows every
 * entry's provider; the vector below owns them, so the registry
 * must be destroyed BEFORE the vector clears. cli_model_config_destroy
 * does this in the right order.
 *
 * `max_turns` is the top-level agent tool-loop cap lifted from
 * models.json; zero means "the file didn't specify one" and main.cpp
 * falls back to its built-in default.
 *
 * `budget` carries the top-level "budget" block (if present); fields
 * with a true mask bit win against xagent's built-in defaults, fields
 * with a false mask bit fall through. Per-model overrides are then
 * merged ON TOP via cli_model_config_resolve_budget().
 */
struct CliModelConfig {
  std::vector<CliModelEntry> entries;
  xAgentModelRegistry        registry    = nullptr; /* owned */
  std::string                default_id;
  int                        max_turns   = 0; /* 0 = use built-in default */
  CliBudgetConf              budget;          /* top-level "budget" block */
};

/* Look up a model entry by id. Returns nullptr when the id is not
 * registered. Linear scan over @p cfg->entries — fine at the
 * handful-of-models scale we expect in practice. Exposed so
 * /model-switch code in the CLI can pull the switched-to entry's
 * budget overrides without reparsing the JSON. */
const CliModelEntry *cli_model_config_find(const CliModelConfig *cfg,
                                           const char           *id);

/* Compose the effective xAgentBudgetConf for a given model, in
 * cascade order: built-in default (everything 0) <- top-level
 * budget block (if mask bit set) <- per-model budget block (if
 * mask bit set). Threshold fields only — policy and the event
 * callback pair are left as zero/null so the caller can splice
 * them back in. @p model_id may be NULL or unknown, in which case
 * only the top-level block contributes. */
xAgentBudgetConf cli_model_config_resolve_budget(const CliModelConfig *cfg,
                                                 const char           *model_id);

/* Load `<data_dir>/models.json` and build a CliModelConfig.
 *
 * Returns 0 on success, non-zero on failure (JSON malformed, unknown
 * "provider", empty model list, or the declared "default" id not
 * present in "models"). On failure @p err_out receives a human-
 * readable diagnostic the caller can print to stderr; the
 * out-parameter @p out is left untouched.
 *
 * A MISSING file (ENOENT) is NOT treated as failure: we still return
 * 0 but with an empty CliModelConfig (entries empty, default_id
 * empty, registry a freshly-created empty registry). As a courtesy
 * we also best-effort-write a template models.json at the expected
 * path so the REPL's "[!] edit <data_dir>/models.json" hint lands
 * the user on a real, pre-structured file — they only need to drop
 * in an API key. The scaffold write is silently skipped if it
 * fails (e.g. read-only data_dir), since the template is a
 * convenience and not a contract. The caller is expected to detect
 * the degraded state via default_id.empty() and drop into a
 * "no model configured" mode so the user can still reach slash
 * commands and fix their configuration from inside the REPL.
 * Every other I/O error (permission denied, short read, …) stays
 * fatal — we'd rather be loud about a file we can see but can't
 * read.
 *
 * The backing providers reuse the shared @p loop / @p http so the
 * host doesn't pay for per-model HTTP clients.
 */
int cli_model_config_load(const char     *data_dir,
                          xEventLoop      loop,
                          xHttpClient     http,
                          CliModelConfig *out,
                          std::string    *err_out);

/* Destroy every provider in @p cfg and the registry that indexes
 * them. Safe to call on a zero-initialised / half-populated cfg;
 * every field is reset to its default afterwards.
 */
void cli_model_config_destroy(CliModelConfig *cfg);

#endif /* MOO_APPS_CLI_CONFIG_H */
