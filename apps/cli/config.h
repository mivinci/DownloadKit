/*
 * Copyright 2025 The xKit Authors. All rights reserved.
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
 *   "models": [
 *     {"id": "kimi",   "kind": "openai",
 *      "model": "kimi-k2.6",
 *      "api_key": "sk-...", "base_url": "https://api.moonshot.cn/v1"},
 *     {"id": "glm",    "kind": "openai",
 *      "model": "glm-4.5",
 *      "api_key": "sk-...", "base_url": "https://open.bigmodel.cn/api/paas/v4"}
 *   ]
 * }
 *
 * Everything is loaded once up front; the returned CliModelConfig
 * bundles both the raw provider handles (for later destruction) and
 * the registry that references them. See cli_model_config_destroy()
 * for the teardown order.
 */

#ifndef XKIT_APPS_CLI_CONFIG_H
#define XKIT_APPS_CLI_CONFIG_H

#include <string>
#include <vector>

#include <xagent/model.h>
#include <xagent/provider.h>
#include <xbase/event.h>
#include <xhttp/client.h>

/* One resolved entry: the concrete provider plus the metadata the
 * CLI wants to surface to the user. `model` is the wire name sent
 * to the provider — it's what /model prints and what the
 * banner shows for the current session. */
struct CliModelEntry {
  std::string    id;        /* registry key, unique                        */
  std::string    kind;      /* "openai" today; future: "anthropic" etc.    */
  std::string    model;     /* wire name sent to the provider              */
  xAgentProvider provider;  /* owned; destroyed in cli_model_config_destroy*/
};

/* Fully-loaded model configuration. The registry borrows every
 * entry's provider; the vector below owns them, so the registry
 * must be destroyed BEFORE the vector clears. cli_model_config_destroy
 * does this in the right order.
 */
struct CliModelConfig {
  std::vector<CliModelEntry> entries;
  xAgentModelRegistry        registry    = nullptr; /* owned */
  std::string                default_id;
};

/* Load `<data_dir>/models.json` and build a CliModelConfig.
 *
 * Returns 0 on success, non-zero on failure (file missing, JSON
 * malformed, unknown "kind", empty model list, or the declared
 * "default" id not present). On failure @p err_out receives a
 * human-readable diagnostic the caller can print to stderr; the
 * out-parameter @p out is left untouched.
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

#endif /* XKIT_APPS_CLI_CONFIG_H */
