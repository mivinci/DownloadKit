/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * config.cpp - Parse <data_dir>/models.json into a CliModelConfig
 *
 * Loads the JSON file, walks the "models" array, creates one
 * xAgentProvider per entry, and packages everything into an
 * xAgentModelRegistry ready to be handed to xAgentCreate. The file
 * format is documented in config.h.
 *
 * All errors are reported as std::string diagnostics via @p err_out
 * so main.cpp can pretty-print them with a stable prefix. On any
 * failure every partially-constructed resource is torn down before
 * returning.
 */

#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>

#include <xagent/provider_openai.h>

namespace {

/* Slurp an entire file into a std::string. Returns true on success.
 * On failure leaves @p out empty and writes a diagnostic to
 * @p err_out. */
bool read_file_all(const std::string &path, std::string *out,
                   std::string *err_out) {
  FILE *fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    if (err_out) {
      *err_out = "cannot open ";
      err_out->append(path);
      err_out->append(": ");
      err_out->append(std::strerror(errno));
    }
    return false;
  }
  if (std::fseek(fp, 0, SEEK_END) != 0) {
    std::fclose(fp);
    if (err_out) *err_out = "fseek failed on " + path;
    return false;
  }
  long sz = std::ftell(fp);
  if (sz < 0) {
    std::fclose(fp);
    if (err_out) *err_out = "ftell failed on " + path;
    return false;
  }
  std::rewind(fp);
  out->resize(static_cast<size_t>(sz));
  size_t n = std::fread(&(*out)[0], 1, static_cast<size_t>(sz), fp);
  std::fclose(fp);
  if (n != static_cast<size_t>(sz)) {
    out->clear();
    if (err_out) *err_out = "short read on " + path;
    return false;
  }
  return true;
}

/* Safely pull a string field from a cJSON object. Returns "" when
 * the field is missing or not a string; the caller decides whether
 * empty is acceptable per field. */
std::string json_string(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!v || !cJSON_IsString(v) || !v->valuestring) return {};
  return std::string(v->valuestring);
}

} /* anonymous namespace */

int cli_model_config_load(const char     *data_dir,
                          xEventLoop      loop,
                          xHttpClient     http,
                          CliModelConfig *out,
                          std::string    *err_out) {
  if (!data_dir || !loop || !http || !out) {
    if (err_out) *err_out = "cli_model_config_load: invalid arguments";
    return -1;
  }

  /* Build the full path: <data_dir>/models.json */
  std::string path(data_dir);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append("models.json");

  std::string text;
  if (!read_file_all(path, &text, err_out)) return -1;

  cJSON *root = cJSON_ParseWithLength(text.c_str(), text.size());
  if (!root) {
    if (err_out) {
      *err_out = "malformed JSON in " + path;
      const char *tail = cJSON_GetErrorPtr();
      if (tail) {
        err_out->append(" (near: ");
        /* Only echo a short excerpt so we don't splatter the user's
         * API keys into stderr if the file is half-baked. */
        err_out->append(tail, std::strlen(tail) < 64 ? std::strlen(tail) : 64);
        err_out->append(")");
      }
    }
    return -1;
  }

  /* Stage the output in locals; only commit to *out once everything
   * succeeds. This keeps cleanup simple on error paths. */
  std::vector<CliModelEntry> entries;
  xAgentModelRegistry        registry = xAgentModelRegistryCreate();
  if (!registry) {
    cJSON_Delete(root);
    if (err_out) *err_out = "out of memory";
    return -1;
  }

  /* Helper lambda: tear down everything we've partially built.
   * Provider destroy must run before registry destroy because the
   * registry borrows providers — but since we ONLY borrow and don't
   * touch providers from Destroy, the order here is actually free;
   * we still follow "registry first, then providers" to mirror the
   * real cli_model_config_destroy() teardown sequence. */
  auto bail = [&](const std::string &msg) -> int {
    if (err_out) *err_out = msg;
    xAgentModelRegistryDestroy(registry);
    for (auto &e : entries) {
      if (e.provider) xAgentProviderDestroy(e.provider);
    }
    cJSON_Delete(root);
    return -1;
  };

  /* Top-level: "default" and "models" are both required. */
  cJSON *default_v = cJSON_GetObjectItemCaseSensitive(root, "default");
  if (!default_v || !cJSON_IsString(default_v) || !default_v->valuestring ||
      !*default_v->valuestring) {
    return bail(path + ": missing or empty \"default\" field");
  }
  std::string default_id = default_v->valuestring;

  cJSON *models_v = cJSON_GetObjectItemCaseSensitive(root, "models");
  if (!models_v || !cJSON_IsArray(models_v)) {
    return bail(path + ": missing or non-array \"models\" field");
  }
  if (cJSON_GetArraySize(models_v) == 0) {
    return bail(path + ": \"models\" array is empty");
  }

  /* Walk each entry and build its provider. */
  cJSON *item = nullptr;
  cJSON_ArrayForEach(item, models_v) {
    if (!cJSON_IsObject(item)) {
      return bail(path + ": every entry in \"models\" must be an object");
    }

    std::string id    = json_string(item, "id");
    std::string kind  = json_string(item, "kind");
    std::string model = json_string(item, "model");
    if (id.empty()) return bail(path + ": model entry missing \"id\"");
    if (kind.empty())
      return bail(path + ": model \"" + id + "\" missing \"kind\"");
    if (model.empty())
      return bail(path + ": model \"" + id + "\" missing \"model\"");

    xAgentProvider pvd = nullptr;

    if (kind == "openai") {
      std::string api_key  = json_string(item, "api_key");
      std::string base_url = json_string(item, "base_url");
      std::string org      = json_string(item, "organization");
      if (api_key.empty())
        return bail(path + ": model \"" + id + "\" missing \"api_key\"");

      xAgentOpenAIConf pconf;
      std::memset(&pconf, 0, sizeof(pconf));
      pconf.api_key       = api_key.c_str();
      pconf.base_url      = base_url.empty() ? nullptr : base_url.c_str();
      pconf.organization  = org.empty() ? nullptr : org.c_str();
      pconf.default_model = model.c_str();
      pconf.timeout_ms    = 60000;
      pvd = xAgentProviderOpenAICreate(loop, http, &pconf);
    } else {
      return bail(path + ": model \"" + id + "\" has unknown kind \"" + kind +
                  "\" (known: openai)");
    }

    if (!pvd)
      return bail(path + ": failed to create provider for \"" + id + "\"");

    CliModelEntry e;
    e.id       = id;
    e.kind     = kind;
    e.model    = model;
    e.provider = pvd;
    entries.push_back(std::move(e));

    /* Register in the registry — entry index equals registry index. */
    xAgentModelSpec spec = {};
    spec.id       = entries.back().id.c_str();
    spec.provider = pvd;
    spec.model    = entries.back().model.c_str();
    xErrno rc = xAgentModelRegistryAdd(registry, &spec);
    if (rc != xErrno_Ok) {
      /* Most likely AlreadyExists — surface a pointed message. */
      return bail(path + ": duplicate or invalid model id \"" + id + "\"");
    }
  }

  /* Validate that the declared default actually exists in the registry. */
  if (!xAgentModelRegistryGet(registry, default_id.c_str())) {
    return bail(path + ": \"default\" = \"" + default_id +
                "\" does not match any entry in \"models\"");
  }

  cJSON_Delete(root);

  /* Commit to the out-parameter. The registry already owns deep
   * copies of every id/model string (see xAgentModelRegistryAdd), so
   * moving / reallocating `entries` afterwards is safe for the
   * registry itself; we keep the entries vector around purely
   * because it owns the providers that the registry borrows. */
  out->entries    = std::move(entries);
  out->registry   = registry;
  out->default_id = std::move(default_id);
  return 0;
}

void cli_model_config_destroy(CliModelConfig *cfg) {
  if (!cfg) return;

  /* Destroy the registry first so no lookups racing against teardown
   * can observe dangling provider pointers. (The cli is
   * single-threaded, so this is defensive rather than strictly
   * required.) */
  if (cfg->registry) {
    xAgentModelRegistryDestroy(cfg->registry);
    cfg->registry = nullptr;
  }

  for (auto &e : cfg->entries) {
    if (e.provider) {
      xAgentProviderDestroy(e.provider);
      e.provider = nullptr;
    }
  }
  cfg->entries.clear();
  cfg->default_id.clear();
}
