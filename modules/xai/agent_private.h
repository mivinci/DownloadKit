/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent_private.h - Concrete layout of xAiAgent for intra-module use
 *
 * The xai module keeps the agent representation intentionally flat:
 * everything in xAiAgentConf is copied by value into struct xAiAgent_,
 * and session.c reaches in directly for provider / tools / limits.
 * Not installed; not part of the public API.
 */

#ifndef XAI_AGENT_PRIVATE_H
#define XAI_AGENT_PRIVATE_H

#include <stddef.h>
#include <xai/agent.h>
#include <xai/provider.h>
#include <xai/tool.h>
#include <xbase/base.h>
#include <xbase/event.h>
#include <xbase/task.h>

struct xAiSession_; /* forward — full definition in session_private.h */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Concrete agent layout.
 *
 * All fields are captured from xAiAgentConf at Create time and are
 * then read-only for the agent's lifetime. Strings and the tools
 * array are borrowed from the caller, as documented in agent.h.
 */
struct xAiAgent_ {

  /**
   * Event loop that owns this agent. Every xAi* API call that
   * touches the agent or any of its sessions must happen on this
   * loop's thread. Never NULL after construction.
   */
  xEventLoop loop;

  /**
   * Backing LLM provider used by all sessions derived from this
   * agent. Borrowed from xAiAgentConf::provider; the caller must
   * keep it alive until every session has been destroyed and
   * xAiAgentDestroy() has been called. Never NULL.
   */
  xAiProvider provider;

  /**
   * Default model identifier sent to the provider when a session
   * does not override it. Borrowed from xAiAgentConf::model; may
   * be NULL, in which case the provider's own default is used.
   */
  const char *model;

  /**
   * Default system prompt injected into every session that does
   * not supply its own. Borrowed from xAiAgentConf::system_prompt;
   * may be NULL.
   */
  const char *system_prompt;

  /**
   * Array of tool definitions available to sessions derived from
   * this agent. Borrowed from xAiAgentConf::tools; individual
   * entries may be shared across multiple agents. May be NULL
   * when no tools are configured.
   */
  const xAiTool **tools;

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
   * override this in their xAiSessionConf inherit the value from
   * here. Zero means "use the library default" (currently 16).
   */
  int max_turns;

  /**
   * Default per-round token cap forwarded to the provider. Sessions
   * that do not override this in their xAiSessionConf inherit the
   * value from here. Zero means "use the provider's default".
   */
  int max_tokens;

  /**
   * Built-in alignment session, or NULL if the agent was created
   * without one (i.e. xAiAgentConf::default_session_conf was NULL).
   * Owned by the agent — destroyed automatically in
   * xAiAgentDestroy(). This session serves as the user's primary
   * conversation entry with the agent and is used for initial
   * alignment conversations where the agent learns user preferences,
   * communication style, and task patterns.
   */
  struct xAiSession_ *default_session;

  /**
   * Unique identifier for this agent instance. Borrowed from
   * xAiAgentConf::agent_id; may be NULL. Used as part of the
   * L1 memory file path.
   */
  const char *agent_id;

  /**
   * Root directory for persistent agent data. Borrowed from
   * xAiAgentConf::data_dir; may be NULL. When both agent_id
   * and data_dir are non-NULL, the agent auto-wires L1
   * persistence into every session it creates.
   */
  const char *data_dir;

  /**
   * Monotonically increasing session counter used to generate
   * session IDs when the caller does not supply one.
   */
  uint64_t session_seq;
};

#ifdef __cplusplus
}
#endif

#endif /* XAI_AGENT_PRIVATE_H */
