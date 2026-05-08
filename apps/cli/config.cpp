/*
 * Copyright 2025 The moo Authors. All rights reserved.
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
 * @p err_out. When the file doesn't exist @p not_found (if provided)
 * is set to true so the caller can distinguish "missing config" —
 * which we now treat as a soft, non-fatal condition that drops the
 * CLI into a degraded "no model" mode — from genuine I/O errors
 * (permission denied, short read, etc.) that stay fatal. */
bool read_file_all(const std::string &path, std::string *out,
                   std::string *err_out, bool *not_found = nullptr) {
  if (not_found) *not_found = false;
  FILE *fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    if (errno == ENOENT && not_found) *not_found = true;
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

/* Pull a non-negative integer field, with a sentinel for "missing".
 * Returns -1 when the key is absent, is not a number, or carries a
 * negative value; the caller decides how to fall back. Any value
 * that exceeds INT_MAX is clamped via the cJSON double path but
 * still passes as "present" — the caller is expected to treat
 * absurdly large numbers the same as a direct user mistake. */
long json_nonneg_int(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!v || !cJSON_IsNumber(v)) return -1;
  double d = v->valuedouble;
  if (d < 0) return -1;
  return static_cast<long>(d);
}

/* Detect the "<...>" placeholders we ship in kModelsJsonTemplate.
 * A value that still looks like <foo-bar> means the user saved the
 * scaffold unchanged (or only partially filled it in). We treat
 * such a field as "not yet configured" rather than "malformed",
 * so the REPL can drop into the same degraded mode it uses when
 * models.json is entirely missing — with the same
 * "edit …/models.json to enable chat" hint — instead of silently
 * constructing a provider that's guaranteed to 401 on first use.
 *
 * The check is intentionally narrow: a single leading '<' and a
 * single trailing '>'. Anything else (real URLs, real keys that
 * happen to contain '<') passes through untouched. */
bool is_placeholder(const std::string &s) {
  return s.size() >= 2 && s.front() == '<' && s.back() == '>';
}

/* Template written to <data_dir>/models.json when the file is
 * missing, to give first-time users something concrete to edit.
 * Mirrors the schema documented in config.h. Fields that MUST be
 * replaced before the config becomes usable are marked with obvious
 * angle-bracket placeholders ("<your-api-key>" etc.). Kept as a
 * raw string literal so the on-disk file matches byte-for-byte
 * what a user would see in the docs. */
constexpr const char *kModelsJsonTemplate =
  "{\n"
  "  \"default\": \"my-model\",\n"
  "  \"max_turns\": 64,\n"
  "  \"models\": [\n"
  "    {\n"
  "      \"id\": \"my-model\",\n"
  "      \"kind\": \"openai\",\n"
  "      \"model\": \"<model-name>\",\n"
  "      \"api_key\": \"<your-api-key>\",\n"
  "      \"base_url\": \"<provider-base-url>\",\n"
  "      \"context_window\": 8192\n"
  "    }\n"
  "  ]\n"
  "}\n";

/* Best-effort: write kModelsJsonTemplate to @p path if we can. Any
 * failure is swallowed on purpose — the template is a convenience,
 * not a contract, and we don't want a read-only data_dir to turn a
 * "please configure me" hint into a hard startup error. The caller
 * always falls through to the "no model configured" degraded mode
 * regardless of whether the scaffold landed. */
void try_write_template(const std::string &path) {
  FILE *fp = std::fopen(path.c_str(), "wb");
  if (!fp) return;
  const size_t len = std::strlen(kModelsJsonTemplate);
  std::fwrite(kModelsJsonTemplate, 1, len, fp);
  std::fclose(fp);
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
  bool        not_found = false;
  if (!read_file_all(path, &text, err_out, &not_found)) {
    if (not_found) {
      /* Missing models.json is soft: instead of failing startup we
       * hand back a valid but empty CliModelConfig (empty registry,
       * empty default_id, no entries). main.cpp detects this by
       * inspecting default_id.empty() and skips agent creation,
       * dropping the REPL into a degraded mode where slash commands
       * still work but chat submits are rejected with a hint to
       * create the file. Clear err_out so we don't leak the
       * "cannot open …" diagnostic: the caller will render its own
       * (warmer) message once it sees the empty config.
       *
       * Before we bail out, scaffold a template models.json at the
       * expected path so the hint ("edit …/models.json to enable
       * chat") lands the user on a real file that's already
       * structured correctly — they only need to paste in an API
       * key. try_write_template is best-effort; any failure here
       * (read-only fs, etc.) is silently ignored. */
      if (err_out) err_out->clear();
      try_write_template(path);
      xAgentModelRegistry registry = xAgentModelRegistryCreate();
      if (!registry) {
        if (err_out) *err_out = "out of memory";
        return -1;
      }
      out->entries.clear();
      out->registry = registry;
      out->default_id.clear();
      return 0;
    }
    return -1;
  }

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

  /* Optional top-level "max_turns": clamps the tool-loop per user
   * input. Missing / non-numeric / negative means "let main.cpp use
   * its built-in default" — we encode that as 0 here. */
  int max_turns = 0;
  {
    long mt = json_nonneg_int(root, "max_turns");
    if (mt > 0) max_turns = static_cast<int>(mt);
  }

  cJSON *models_v = cJSON_GetObjectItemCaseSensitive(root, "models");
  if (!models_v || !cJSON_IsArray(models_v)) {
    return bail(path + ": missing or non-array \"models\" field");
  }
  if (cJSON_GetArraySize(models_v) == 0) {
    return bail(path + ": \"models\" array is empty");
  }

  /* Track whether any entry still carries template placeholders.
   * If so, we abandon the partially-built registry after the walk
   * and return the same empty CliModelConfig we produce for a
   * missing file, so main.cpp renders exactly one consistent
   * "no model is configured" hint regardless of whether the file
   * is missing, freshly scaffolded, or half-filled-in. */
  bool has_placeholder = false;

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
    if (is_placeholder(model)) has_placeholder = true;

    xAgentProvider pvd = nullptr;

    if (kind == "openai") {
      std::string api_key  = json_string(item, "api_key");
      std::string base_url = json_string(item, "base_url");
      std::string org      = json_string(item, "organization");
      if (api_key.empty())
        return bail(path + ": model \"" + id + "\" missing \"api_key\"");
      if (is_placeholder(api_key) || is_placeholder(base_url))
        has_placeholder = true;

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

    /* Optional per-entry "context_window": overrides the session's
     * budget.context_window whenever this model is active. Missing or
     * non-positive means "use the global default set up in main.cpp",
     * which we encode as 0 so the caller can distinguish explicit
     * from absent. */
    size_t context_window = 0;
    {
      long cw = json_nonneg_int(item, "context_window");
      if (cw > 0) context_window = static_cast<size_t>(cw);
    }

    CliModelEntry e;
    e.id             = id;
    e.kind           = kind;
    e.model          = model;
    e.context_window = context_window;
    e.provider       = pvd;
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

  /* If any entry still carries <placeholders>, the file parses but
   * isn't actually usable. Tear down everything we built and return
   * the same empty CliModelConfig we produce for a missing file —
   * main.cpp detects default_id.empty() and renders the standard
   * "edit …/models.json to enable chat" hint, rather than letting
   * the user submit a chat turn that would instantly 401. err_out
   * is cleared for the same reason as in the ENOENT branch. */
  if (has_placeholder) {
    xAgentModelRegistryDestroy(registry);
    for (auto &e : entries) {
      if (e.provider) xAgentProviderDestroy(e.provider);
    }
    if (err_out) err_out->clear();
    xAgentModelRegistry empty = xAgentModelRegistryCreate();
    if (!empty) {
      if (err_out) *err_out = "out of memory";
      return -1;
    }
    out->entries.clear();
    out->registry = empty;
    out->default_id.clear();
    return 0;
  }

  /* Commit to the out-parameter. The registry already owns deep
   * copies of every id/model string (see xAgentModelRegistryAdd), so
   * moving / reallocating `entries` afterwards is safe for the
   * registry itself; we keep the entries vector around purely
   * because it owns the providers that the registry borrows. */
  out->entries    = std::move(entries);
  out->registry   = registry;
  out->default_id = std::move(default_id);
  out->max_turns  = max_turns;
  return 0;
}

const CliModelEntry *cli_model_config_find(const CliModelConfig *cfg,
                                           const char           *id) {
  if (!cfg || !id || !*id) return nullptr;
  for (const auto &e : cfg->entries) {
    if (e.id == id) return &e;
  }
  return nullptr;
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
  cfg->max_turns = 0;
}
