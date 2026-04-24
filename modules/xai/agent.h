/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent.h - Agent capability template for the xai core
 *
 * An xAiAgent bundles together everything that defines an AI
 * "character" independent of any particular conversation: which
 * provider to talk to, which model to prefer, the system prompt,
 * the tool set, the task pool used for tool execution, and the
 * loop-wide limits (max turns, token budget).
 *
 * One agent can mint many xAiSession instances (see <xai/session.h>),
 * which are the stateful per-conversation objects. The agent itself
 * holds no conversation history; it is long-lived and safe to share
 * across sessions that live on the same event loop.
 *
 * Threading:
 * - Every xAi* API call must happen on the event loop thread that
 *   owns the agent (the @p loop field in xAiAgentConf). Tool
 *   handlers declared concurrent_safe may run on the agent's task
 *   pool threads; everything else runs on @p loop.
 */

#ifndef XAI_AGENT_H
#define XAI_AGENT_H

#include <stddef.h>
#include <xai/provider.h>
#include <xai/tool.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xbase/task.h>

/**
 * @brief Opaque handle to an agent instance.
 */
XDEF_HANDLE(xAiAgent);

/**
 * @brief Configuration for creating an agent.
 *
 * Zero-initialise for defaults where noted. Fields marked "borrowed"
 * are referenced by the agent for its entire lifetime; the caller
 * must keep them alive and outlive every session derived from the
 * agent.
 */
XDEF_STRUCT(xAiAgentConf) {
  xEventLoop loop;       /**< Event loop owning the agent
                              (must not be NULL).                        */
  xAiProvider provider;  /**< LLM provider to drive (borrowed, must not
                              be NULL).                                  */

  const char *model;     /**< Default model id, may be NULL to fall back
                              to the provider's default.                 */
  const char *system_prompt; /**< Default system prompt (borrowed, may
                                  be NULL).                              */

  const xAiTool **tools; /**< Tool array (borrowed, may be NULL).
                              Individual tools may be shared across
                              multiple agents.                           */
  size_t n_tools;        /**< Number of entries in @p tools              */

  xTaskGroup task_group; /**< Worker pool used to run concurrent-safe
                              tool handlers. May be NULL, in which case
                              all tool handlers run on @p loop.
                              Pass xTaskGroupGlobal() to share the
                              process-wide pool.                         */

  int    max_turns;      /**< Hard cap on LLM round-trips per session
                              run. 0 = library default.                  */
  int    max_tokens;     /**< Per-round token cap forwarded to the
                              provider. 0 = provider default.            */
  size_t context_budget; /**< Soft upper bound (bytes) on serialized
                              conversation history before the session
                              triggers compression / truncation.
                              0 = library default.                       */
};

/**
 * @brief Create an agent from the given configuration.
 *
 * The agent does not take ownership of @p conf->provider,
 * @p conf->tools or @p conf->task_pool; the caller keeps them alive
 * until every session derived from this agent has been destroyed and
 * finally xAiAgentDestroy() is called.
 *
 * @param conf  Agent configuration (must not be NULL, conf->loop and
 *              conf->provider must not be NULL).
 * @return      A new agent handle, or NULL on failure.
 */
XCAPI(xAiAgent) xAiAgentCreate(const xAiAgentConf *conf);

/**
 * @brief Destroy an agent and release its resources.
 *
 * The caller must ensure every session derived from @p agent has
 * already been destroyed.
 *
 * @param agent  Agent handle (NULL is a no-op).
 */
XCAPI(void) xAiAgentDestroy(xAiAgent agent);

#endif /* XAI_AGENT_H */
