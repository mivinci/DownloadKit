/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent.h - Agent capability template for the xai core
 *
 * An xAgent bundles together everything that defines an AI
 * "character" independent of any particular conversation: which
 * provider to talk to, which model to prefer, the system prompt,
 * the tool set, the task pool used for tool execution, and the
 * loop-wide limits (max turns, token budget).
 *
 * One agent can mint many xAgentSession instances (see <xagent/session.h>),
 * which are the stateful per-conversation objects. The agent itself
 * holds no conversation history; it is long-lived and safe to share
 * across sessions that live on the same event loop.
 *
 * Threading:
 * - Every xAgent* API call must happen on the event loop thread that
 *   owns the agent (the @p loop field in xAgentConf). Tool
 *   handlers declared concurrent_safe may run on the agent's task
 *   pool threads; everything else runs on @p loop.
 */

#ifndef XAGENT_AGENT_H
#define XAGENT_AGENT_H

#include <stddef.h>
#include <xagent/model.h>
#include <xagent/provider.h>
#include <xagent/tool.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xbase/task.h>

/**
 * @brief Forward handle for the pluggable memory store.
 *
 * The full API lives in <xagent/memory.h>; we forward-declare only
 * the handle here to keep agent.h from pulling memory.h (which
 * itself depends on session.h, which depends on agent.h, creating
 * a cycle). C11 allows this same typedef to appear here and in
 * memory.h without conflict.
 */
XDEF_HANDLE(xAgentMemory);

/**
 * @brief Opaque handle to a session instance.
 *
 * Forward-declared here for xAgentCreateSession(); the full
 * definition lives in <xagent/session.h>.
 */
XDEF_HANDLE(xAgentSession);

/**
 * @brief Forward declaration of session configuration.
 *
 * Defined in <xagent/session.h>; forward-declared here so
 * xAgentCreateSession() can accept it without creating a
 * circular include (session.h already includes agent.h).
 */
XDEF_STRUCT(xAgentSessionConf);

/**
 * @brief Opaque handle to an agent instance.
 */
XDEF_HANDLE(xAgent);

/**
 * @brief Configuration for creating an agent.
 *
 * Zero-initialise for defaults where noted. Fields marked "borrowed"
 * are referenced by the agent for its entire lifetime; the caller
 * must keep them alive and outlive every session derived from the
 * agent.
 */
XDEF_STRUCT(xAgentConf) {

  /**
   * @brief Event loop that owns the agent.
   *
   * Every xAgent* API call that touches this agent or any of its
   * sessions must happen on this loop's thread. Must not be NULL.
   */
  xEventLoop loop;

  /**
   * @brief LLM provider to drive conversations.
   *
   * Borrowed from the caller; the agent does not take ownership.
   * Must remain alive until every session has been destroyed and
   * xAgentDestroy() has been called.
   *
   * Exactly one of the two provider-selection paths must be used:
   *   - LEGACY: set @ref provider (and optionally @ref model) to
   *     bind the agent to a single backend for its whole lifetime.
   *   - REGISTRY: leave @ref provider NULL and set
   *     @ref model_registry + @ref default_model_id; the agent
   *     looks up the default spec at create time and also keeps
   *     a borrowed reference to the registry so sessions can
   *     switch models at runtime via xAgentSessionSetModel().
   *
   * Mixing the two is an error (xAgentCreate returns NULL).
   */
  xAgentProvider provider;

  /**
   * @brief Default model identifier (LEGACY path only).
   *
   * May be NULL to fall back to the provider's default model.
   * Borrowed from the caller; sessions that do not override this
   * in their xAgentSessionConf inherit the value from here.
   * Ignored when @ref model_registry is non-NULL.
   */
  const char *model;

  /**
   * @brief Model registry (REGISTRY path).
   *
   * Borrowed from the caller; must outlive every agent and session
   * that uses it. When non-NULL the agent resolves
   * @ref default_model_id against it at create time to populate
   * its active provider+model, and retains the pointer so
   * xAgentSessionSetModel() can flip sessions between registered
   * specs at runtime. NULL selects the LEGACY path.
   */
  xAgentModelRegistry model_registry;

  /**
   * @brief Key into @ref model_registry for the agent's default
   *        spec.
   *
   * Must be non-NULL, non-empty, and resolve to an entry in
   * @ref model_registry when the latter is non-NULL. Ignored
   * when @ref model_registry is NULL (LEGACY path).
   */
  const char *default_model_id;

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
  const xAgentTool **tools;

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
   * xAgentSessionConf. Zero means "use the library default".
   */
  int max_turns;

  /**
   * @brief Default per-round token cap forwarded to the provider.
   *
   * Inherited by sessions that don't override this in their
   * xAgentSessionConf. Zero means "use the provider's default".
   */
  int max_tokens;

  /**
   * @brief Configuration template for the agent's built-in default
   *        session (borrowed, may be NULL).
   *
   * The agent owns a long-lived xAgentSession — the user's primary
   * conversation entry — that lives for the agent's entire lifetime
   * and is destroyed automatically by xAgentDestroy(). This
   * session is used for:
   *
   *   - The default conversation context where the user interacts
   *     with the agent (origin defaults to xAgentInputOrigin_User).
   *   - Initial alignment conversations where the agent learns
   *     user preferences, communication style, and task patterns.
   *   - A stable, always-available conversation context that
   *     higher layers can fall back to when no user-driven session
   *     is active.
   *
   * When non-NULL the pointed-to struct is copied by value at
   * xAgentCreate time (the caller may free it afterwards).
   * The origin field is honoured as-is — zero-initialised configs
   * default to xAgentInputOrigin_User, making the default session a
   * user-driven conversation. system_prompt, model, budget, etc.
   * are also honoured as-is. When NULL the default session is
   * created with a zero-initialised xAgentSessionConf (all fields
   * inherited from the agent).
   */
  const xAgentSessionConf *default_session_conf;

  /**
   * @brief Pluggable long-term memory store.
   *
   * Borrowed from the caller; must remain alive for the agent's
   * lifetime and be destroyed AFTER xAgentDestroy() returns. When
   * non-NULL the agent:
   *
   *   - Auto-wires an L1 preserve callback into every session it
   *     creates so each session's history is appended to the store
   *     via xAgentMemoryAppend().
   *   - On xAgentCreateSession(), primes the new session's history
   *     from the store so resumed sessions pick up where they left
   *     off.
   *
   * NULL (the default) disables both — sessions run in-memory only
   * and no cross-run persistence happens. Callers who want a
   * file-backed store can use xAgentMemoryJsonlCreate().
   */
  xAgentMemory memory;

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
 * finally xAgentDestroy() is called.
 *
 * If @p conf->default_session_conf is non-NULL the agent creates an
 * internal "default session" at construction time (see
 * xAgentDefaultSession()). The default session is destroyed
 * automatically when the agent is destroyed.
 *
 * @param conf  Agent configuration (must not be NULL, conf->loop
 *              must not be NULL). Exactly one of the following
 *              must be provided:
 *                - conf->provider non-NULL (LEGACY path), OR
 *                - conf->model_registry non-NULL AND
 *                  conf->default_model_id non-NULL and resolvable
 *                  (REGISTRY path).
 * @return      A new agent handle, or NULL on failure.
 */
XCAPI(xAgent) xAgentCreate(const xAgentConf *conf);

/**
 * @brief Destroy an agent and release its resources.
 *
 * The caller must ensure every session derived from @p agent has
 * already been destroyed (except the agent's built-in default
 * session, which is destroyed automatically).
 *
 * @param agent  Agent handle (NULL is a no-op).
 */
XCAPI(void) xAgentDestroy(xAgent agent);

/**
 * @brief Return the agent's built-in default session.
 *
 * The default session is a long-lived conversation that exists for
 * the agent's entire lifetime. It is created (if the agent was
 * configured with a non-NULL @ref xAgentConf::default_session_conf)
 * during xAgentCreate and destroyed during xAgentDestroy.
 *
 * The default session's origin defaults to
 * @ref xAgentInputOrigin_User — it is the user's primary conversation
 * entry with the agent. The caller may override the origin in the
 * @ref xAgentConf::default_session_conf if a different origin is
 * desired.
 *
 * Callers may feed user alignment messages into this session
 * (xAgentSessionInput), read its history, or inspect its usage — but
 * must NOT destroy it. The session is valid until
 * xAgentDestroy() is called on @p agent.
 *
 * @param agent  Agent handle.
 * @return       The default session, or NULL if the agent has none
 *               (i.e. @ref xAgentConf::default_session_conf was
 *               NULL at creation time and no default session was
 *               created).
 */
XCAPI(xAgentSession) xAgentDefaultSession(xAgent agent);

/**
 * @brief Create a session bound to the agent.
 *
 * This is the agent-scoped counterpart to xAgentSessionCreate().
 * The returned session is fully initialised; the caller should use
 * it exactly like one created via xAgentSessionCreate().
 *
 * @param agent  Agent to derive the session from (must not be NULL).
 * @param conf   Session configuration (must not be NULL; conf->cbs.on_done
 *               should usually be set).
 * @return       A new session handle, or NULL on failure.
 */
XCAPI(xAgentSession) xAgentCreateSession(xAgent              agent,
                                        const xAgentSessionConf *conf);

#endif /* XAGENT_AGENT_H */
