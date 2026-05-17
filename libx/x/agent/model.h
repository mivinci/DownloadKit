/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * model.h - Model registry for the xagent core
 *
 * A ModelRegistry is a lookup table keyed by a caller-defined
 * identifier, where each entry ("model spec") pairs a borrowed
 * xAgentProvider with the wire-level model name to send and an
 * optional set of default sampling parameters.
 *
 * The registry lets a host (typically the CLI app) declare several
 * "how to call the LLM" bundles at startup and then:
 *
 *   1. Point an xAgent at a default one via xAgentConf::model_registry
 *      + xAgentConf::default_model_id.
 *   2. Let an xAgentSession switch between them at runtime via
 *      xAgentSessionSetModel(), without recreating the agent or any
 *      provider.
 *   3. Let agent-internal subsystems (future: hierarchical memory,
 *      summary, plan) pick different specs per job by id.
 *
 * The registry only **borrows** provider handles — it does not own
 * them. The host remains responsible for creating providers before
 * populating the registry and for destroying them after every agent
 * / session that uses the registry has been torn down.
 */

#ifndef XAGENT_MODEL_H
#define XAGENT_MODEL_H

#include <stddef.h>
#include <x/agent/provider.h>
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Opaque handle to a model registry.
 */
XDEF_HANDLE(xAgentModelRegistry);

/**
 * @brief One entry in an xAgentModelRegistry.
 *
 * Caller fills this in and passes it to xAgentModelRegistryAdd.
 * The registry deep-copies @p id and @p model; @p provider is
 * borrowed (must outlive the registry and every agent/session that
 * looks up this entry).
 */
XDEF_STRUCT(xAgentModelSpec) {
  /**
   * @brief Registry lookup key. Must be non-NULL, non-empty and
   *        unique within the registry. Duplicated internally.
   */
  const char *id;

  /**
   * @brief Backing provider. Borrowed; must not be NULL. Must
   *        outlive every consumer that resolves this spec.
   */
  xAgentProvider provider;

  /**
   * @brief Wire-level model name forwarded to the provider on every
   *        submit() (e.g. "kimi-k2.6", "glm-4.5", "gpt-4o-mini").
   *        May be NULL, in which case the provider's own default
   *        is used. Duplicated internally.
   */
  const char *model;

  /**
   * @brief Optional default temperature. A negative value means
   *        "not set" — the provider's own default is used.
   *        Currently informational: xAgentCreate / xAgentSessionSetModel
   *        do not yet thread sampling parameters through, but the
   *        field is reserved so future work can do so without
   *        breaking ABI.
   */
  double temperature;

  /**
   * @brief Optional default max_tokens. Zero means "not set".
   *        See @ref temperature for the forward-compat caveat.
   */
  int max_tokens;
};

/**
 * @brief Create an empty model registry.
 * @return A new registry, or NULL on allocation failure.
 */
XCAPI(xAgentModelRegistry) xAgentModelRegistryCreate(void);

/**
 * @brief Destroy a registry.
 *
 * Releases the registry's own bookkeeping (the entry table and every
 * duplicated id/model string). Does NOT touch borrowed providers;
 * the caller is responsible for destroying those separately after
 * every consumer of the registry has been torn down.
 *
 * @param reg  Registry handle (NULL is a no-op).
 */
XCAPI(void) xAgentModelRegistryDestroy(xAgentModelRegistry reg);

/**
 * @brief Add a model spec to the registry.
 *
 * The @p spec is deep-copied internally (id and model strings are
 * duplicated); the caller may free / reuse @p spec after this call.
 * The @p spec->provider handle is borrowed.
 *
 * @param reg   Registry handle.
 * @param spec  Spec to add. @p spec->id must be a non-empty string
 *              that is not already present, and @p spec->provider
 *              must be non-NULL.
 * @return      xErrno_Ok on success; xErrno_InvalidArg for malformed
 *              arguments (NULL reg, NULL/empty id, NULL provider);
 *              xErrno_AlreadyExists when @p spec->id already lives
 *              in the registry; xErrno_NoMemory on allocation failure.
 */
XCAPI(xErrno) xAgentModelRegistryAdd(xAgentModelRegistry reg, const xAgentModelSpec *spec);

/**
 * @brief Look up a spec by id.
 *
 * @param reg  Registry handle.
 * @param id   Lookup key (NUL-terminated).
 * @return     Borrowed pointer to the stored spec (fields remain
 *             valid until the registry is destroyed or the entry
 *             is replaced), or NULL when @p reg is NULL, @p id is
 *             NULL, or the id is not registered.
 */
XCAPI(const xAgentModelSpec *)
xAgentModelRegistryGet(xAgentModelRegistry reg, const char *id);

/**
 * @brief Number of entries currently in the registry.
 */
XCAPI(size_t) xAgentModelRegistryCount(xAgentModelRegistry reg);

/**
 * @brief Indexed access to registered specs (order is insertion
 *        order).
 *
 * Useful for CLI commands like "/model" that want to list every
 * known id. Returns NULL if @p reg is NULL or @p idx is out of
 * range.
 */
XCAPI(const xAgentModelSpec *)
xAgentModelRegistryAt(xAgentModelRegistry reg, size_t idx);

#endif /* XAGENT_MODEL_H */
