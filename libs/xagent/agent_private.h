/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent_private.h - Concrete layout of xAgent for intra-module use
 *
 * The xai module keeps the agent representation intentionally flat:
 * everything in xAgentConf is copied by value into struct xAgent_,
 * and session.c reaches in directly for provider / tools / limits.
 * Not installed; not part of the public API.
 */

#ifndef XAGENT_AGENT_PRIVATE_H
#define XAGENT_AGENT_PRIVATE_H

#include <stddef.h>
#include <xagent/agent.h>
#include <xagent/model.h>
#include <xagent/provider.h>
#include <xagent/tool.h>
#include <xbase/base.h>
#include <xbase/event.h>
#include <xbase/task.h>

struct xAgentSession_; /* forward — full definition in session_private.h */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Concrete agent layout.
 *
 * All fields are captured from xAgentConf at Create time and are
 * then read-only for the agent's lifetime. Strings and the tools
 * array are borrowed from the caller, as documented in agent.h.
 */
struct xAgent_ {

  /**
   * Event loop that owns this agent. Every xAgent* API call that
   * touches the agent or any of its sessions must happen on this
   * loop's thread. Never NULL after construction.
   */
  xEventLoop loop;

  /**
   * Backing LLM provider used by all sessions derived from this
   * agent. Borrowed from xAgentConf::provider; the caller must
   * keep it alive until every session has been destroyed and
   * xAgentDestroy() has been called. Never NULL.
   */
  xAgentProvider provider;

  /**
   * Default model identifier sent to the provider when a session
   * does not override it. Borrowed from xAgentConf::model; may
   * be NULL, in which case the provider's own default is used.
   */
  const char *model;

  /**
   * Default system prompt injected into every session that does
   * not supply its own. Borrowed from xAgentConf::system_prompt;
   * may be NULL.
   */
  const char *system_prompt;

  /**
   * Model registry the agent was configured against, or NULL when
   * the agent was created via the legacy single-provider path
   * (xAgentConf::provider set directly). Borrowed from
   * xAgentConf::model_registry; must outlive the agent. Consulted
   * by xAgentSessionSetModel() to resolve an id -> (provider, model)
   * at runtime, and available to future agent-internal subsystems
   * (hierarchical memory, summary, plan) that want to pick a
   * different spec per job.
   */
  xAgentModelRegistry model_registry;

  /**
   * Array of tool definitions available to sessions derived from
   * this agent. Borrowed from xAgentConf::tools; individual
   * entries may be shared across multiple agents. May be NULL
   * when no tools are configured.
   */
  const xAgentTool **tools;

  /**
   * Number of entries in the @ref tools array. Zero when no tools
   * are configured.
   */
  size_t tools_count;

  /**
   * Optional worker pool used to execute tool handlers that are
   * marked concurrent_safe. May be NULL, in which case all tool
   * handlers run on the agent's event loop. Pass xTaskGroupGlobal()
   * to share the process-wide pool.
   */
  xTaskGroup task_group;

  /**
   * Default per-session LLM round-trip cap. Sessions that do not
   * override this in their xAgentSessionConf inherit the value from
   * here. Zero means "use the library default" (currently 16).
   */
  int max_turns;

  /**
   * Default per-round token cap forwarded to the provider. Sessions
   * that do not override this in their xAgentSessionConf inherit the
   * value from here. Zero means "use the provider's default".
   */
  int max_tokens;

  /**
   * Built-in alignment session, or NULL if the agent was created
   * without one (i.e. xAgentConf::default_session_conf was NULL).
   * Owned by the agent — destroyed automatically in
   * xAgentDestroy(). This session serves as the user's primary
   * conversation entry with the agent and is used for initial
   * alignment conversations where the agent learns user preferences,
   * communication style, and task patterns.
   */
  struct xAgentSession_ *default_session;

  /**
   * Unique identifier for this agent instance. Borrowed from
   * xAgentConf::agent_id; may be NULL. Used as part of the
   * L1 memory file path.
   */
  const char *agent_id;

  /**
   * Root directory for persistent agent data. Borrowed from
   * xAgentConf::data_dir; may be NULL. When both agent_id
   * and data_dir are non-NULL, the agent auto-wires L1
   * persistence into every session it creates.
   */
  const char *data_dir;

  /**
   * Whether this agent may launch sidecar Queries. Copied from
   * xAgentConf::enable_sidecar_query; zero means disabled.
   * Session code checks this before arming the sidecar idle
   * timer, so the entire sidecar subsystem is gated by this
   * single flag.
   */
  int enable_sidecar_query;

  /**
   * Monotonically increasing session counter used to generate
   * session IDs when the caller does not supply one.
   */
  uint64_t session_seq;
};

#ifdef __cplusplus
}
#endif

#endif /* XAGENT_AGENT_PRIVATE_H */
