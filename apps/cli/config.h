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
 *   "models": [
 *     {"id": "kimi",   "kind": "openai",
 *      "model": "kimi-k2.6",
 *      "api_key": "sk-...", "base_url": "https://api.moonshot.cn/v1",
 *      "context_window": 131072},
 *     {"id": "glm",    "kind": "openai",
 *      "model": "glm-4.5",
 *      "api_key": "sk-...", "base_url": "https://open.bigmodel.cn/api/paas/v4"}
 *   ]
 * }
 *
 * The top-level "max_turns" caps the tool-loop length per user input
 * (default: 64). Per-model "context_window" sets the token budget the
 * session enforces on the rolling history (default: 8192); on
 * /model switches the CLI refreshes the session's budget so large and
 * small models don't share the same window. Both fields are optional.
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

#include <xagent/model.h>
#include <xagent/provider.h>
#include <xbase/event.h>
#include <xhttp/client.h>

/* One resolved entry: the concrete provider plus the metadata the
 * CLI wants to surface to the user. `model` is the wire name sent
 * to the provider — it's what /model prints and what the
 * banner shows for the current session. `context_window` mirrors
 * the per-model token ceiling the session's budget gate should
 * enforce; zero means "use the global default" (see config.cpp). */
struct CliModelEntry {
  std::string    id;        /* registry key, unique                        */
  std::string    kind;      /* "openai" today; future: "anthropic" etc.    */
  std::string    model;     /* wire name sent to the provider              */
  size_t         context_window = 0; /* tokens; 0 = fall back to default   */
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
 */
struct CliModelConfig {
  std::vector<CliModelEntry> entries;
  xAgentModelRegistry        registry    = nullptr; /* owned */
  std::string                default_id;
  int                        max_turns   = 0; /* 0 = use built-in default */
};

/* Look up a model entry by id. Returns nullptr when the id is not
 * registered. Linear scan over @p cfg->entries — fine at the
 * handful-of-models scale we expect in practice. Exposed so
 * /model-switch code in the CLI can pull the switched-to entry's
 * context_window without reparsing the JSON. */
const CliModelEntry *cli_model_config_find(const CliModelConfig *cfg,
                                           const char           *id);

/* Load `<data_dir>/models.json` and build a CliModelConfig.
 *
 * Returns 0 on success, non-zero on failure (JSON malformed, unknown
 * "kind", empty model list, or the declared "default" id not present
 * in "models"). On failure @p err_out receives a human-readable
 * diagnostic the caller can print to stderr; the out-parameter @p
 * out is left untouched.
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
