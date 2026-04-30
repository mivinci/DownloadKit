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
 * @brief Opaque handle to a session instance.
 *
 * Forward-declared here for xAiAgentCreateSession(); the full
 * definition lives in <xai/session.h>.
 */
XDEF_HANDLE(xAiSession);

/**
 * @brief Forward declaration of session configuration.
 *
 * Defined in <xai/session.h>; forward-declared here so
 * xAiAgentCreateSession() can accept it without creating a
 * circular include (session.h already includes agent.h).
 */
XDEF_STRUCT(xAiSessionConf);

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

  /**
   * @brief Event loop that owns the agent.
   *
   * Every xAi* API call that touches this agent or any of its
   * sessions must happen on this loop's thread. Must not be NULL.
   */
  xEventLoop loop;

  /**
   * @brief LLM provider to drive conversations.
   *
   * Borrowed from the caller; the agent does not take ownership.
   * Must not be NULL. Must remain alive until every session has
   * been destroyed and xAiAgentDestroy() has been called.
   */
  xAiProvider provider;

  /**
   * @brief Default model identifier.
   *
   * May be NULL to fall back to the provider's default model.
   * Borrowed from the caller; sessions that do not override this
   * in their xAiSessionConf inherit the value from here.
   */
  const char *model;

  /**
   * @brief Base persona description (system prompt).
   *
   * Borrowed from the caller; may be NULL. This field is the raw
   * template — the final system prompt seen by the session may
   * differ.
   */
  const char *system_prompt;

  /**
   * @brief Array of tool definitions available to the agent.
   *
   * Borrowed from the caller; individual tools may be shared across
   * multiple agents. May be NULL when no tools are needed.
   */
  const xAiTool **tools;

  /**
   * @brief Number of entries in the @ref tools array.
   */
  size_t tools_count;

  /**
   * @brief Worker pool for concurrent-safe tool handlers.
   *
   * May be NULL, in which case all tool handlers run on the agent's
   * event loop (@ref loop). Pass xTaskGroupGlobal() to share the
   * process-wide pool.
   */
  xTaskGroup task_group;

  /**
   * @brief Default per-session LLM round-trip cap.
   *
   * Inherited by sessions that don't override this in their
   * xAiSessionConf. Zero means "use the library default".
   */
  int max_turns;

  /**
   * @brief Default per-round token cap forwarded to the provider.
   *
   * Inherited by sessions that don't override this in their
   * xAiSessionConf. Zero means "use the provider's default".
   */
  int max_tokens;

  /**
   * @brief Configuration template for the agent's built-in default
   *        session (borrowed, may be NULL).
   *
   * The agent owns a long-lived xAiSession — the user's primary
   * conversation entry — that lives for the agent's entire lifetime
   * and is destroyed automatically by xAiAgentDestroy(). This
   * session is used for:
   *
   *   - The default conversation context where the user interacts
   *     with the agent (origin defaults to xAiInputOrigin_User).
   *   - Initial alignment conversations where the agent learns
   *     user preferences, communication style, and task patterns.
   *   - A stable, always-available conversation context that
   *     higher layers can fall back to when no user-driven session
   *     is active.
   *
   * When non-NULL the pointed-to struct is copied by value at
   * xAiAgentCreate time (the caller may free it afterwards).
   * The origin field is honoured as-is — zero-initialised configs
   * default to xAiInputOrigin_User, making the default session a
   * user-driven conversation. system_prompt, model, budget, etc.
   * are also honoured as-is. When NULL the default session is
   * created with a zero-initialised xAiSessionConf (all fields
   * inherited from the agent).
   */
  const xAiSessionConf *default_session_conf;

  /**
   * @brief Unique identifier for this agent instance.
   *
   * Used as part of the L1 memory file path:
   *   {data_dir}/agents/{agent_id}/sessions/{session_id}/memory.jsonl
   * Borrowed from the caller; must remain alive for the agent's
   * lifetime. May be NULL — when NULL the agent does not
   * auto-wire the L1 preserve callback into its sessions.
   */
  const char *agent_id;

  /**
   * @brief Root directory for persistent agent data.
   *
   * Borrowed from the caller; must remain alive for the agent's
   * lifetime. When non-NULL and @ref agent_id is non-NULL, the
   * agent automatically wires an L1 preserve callback into every
   * session it creates, appending JSONL entries under:
   *   {data_dir}/agents/{agent_id}/sessions/{session_id}/memory.jsonl
   * May be NULL — when NULL the agent does not auto-wire the
   * L1 preserve callback.
   */
  const char *data_dir;

  /**
   * @brief Whether the agent may launch sidecar Queries when an
   *        async tool call goes idle.
   *
   * When true (non-zero), sessions derived from this agent will
   * monitor async tool output for stalls; if no output arrives
   * within the session's sidecar_idle_ms, a lightweight sidecar
   * Query is launched to inspect the situation and optionally
   * send input (e.g. shell_stdin) to the stalled command.
   * When false (zero, the default), no sidecar is ever created.
   */
  int enable_sidecar_query;
};

/**
 * @brief Create an agent from the given configuration.
 *
 * The agent does not take ownership of @p conf->provider,
 * @p conf->tools or @p conf->task_pool; the caller keeps them alive
 * until every session derived from this agent has been destroyed and
 * finally xAiAgentDestroy() is called.
 *
 * If @p conf->default_session_conf is non-NULL the agent creates an
 * internal "default session" at construction time (see
 * xAiAgentDefaultSession()). The default session is destroyed
 * automatically when the agent is destroyed.
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
 * already been destroyed (except the agent's built-in default
 * session, which is destroyed automatically).
 *
 * @param agent  Agent handle (NULL is a no-op).
 */
XCAPI(void) xAiAgentDestroy(xAiAgent agent);

/**
 * @brief Return the agent's built-in default session.
 *
 * The default session is a long-lived conversation that exists for
 * the agent's entire lifetime. It is created (if the agent was
 * configured with a non-NULL @ref xAiAgentConf::default_session_conf)
 * during xAiAgentCreate and destroyed during xAiAgentDestroy.
 *
 * The default session's origin defaults to
 * @ref xAiInputOrigin_User — it is the user's primary conversation
 * entry with the agent. The caller may override the origin in the
 * @ref xAiAgentConf::default_session_conf if a different origin is
 * desired.
 *
 * Callers may feed user alignment messages into this session
 * (xAiSessionInput), read its history, or inspect its usage — but
 * must NOT destroy it. The session is valid until
 * xAiAgentDestroy() is called on @p agent.
 *
 * @param agent  Agent handle.
 * @return       The default session, or NULL if the agent has none
 *               (i.e. @ref xAiAgentConf::default_session_conf was
 *               NULL at creation time and no default session was
 *               created).
 */
XCAPI(xAiSession) xAiAgentDefaultSession(xAiAgent agent);

/**
 * @brief Return the agent's unique identifier.
 *
 * Returns the @ref xAiAgentConf::agent_id string that was provided
 * at creation time. NULL if the agent was created without one.
 *
 * @param agent  Agent handle.
 * @return       The agent id, or NULL.
 */
XCAPI(const char *) xAiAgentId(xAiAgent agent);

/**
 * @brief Create a session bound to the agent.
 *
 * This is the agent-scoped counterpart to xAiSessionCreate().
 * The returned session is fully initialised; the caller should use
 * it exactly like one created via xAiSessionCreate().
 *
 * @param agent  Agent to derive the session from (must not be NULL).
 * @param conf   Session configuration (must not be NULL; conf->cbs.on_done
 *               should usually be set).
 * @return       A new session handle, or NULL on failure.
 */
XCAPI(xAiSession) xAiAgentCreateSession(xAiAgent              agent,
                                        const xAiSessionConf *conf);

#endif /* XAI_AGENT_H */
