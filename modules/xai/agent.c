/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent.c - Capability-template bookkeeping for the xai core
 *
 * An agent is pure state: every field in xAiAgentConf is copied into
 * struct xAiAgent_ and never mutated thereafter. No event loop state,
 * no provider calls, no tool dispatch — that all lives in session.c.
 *
 * The caller retains ownership of everything the agent borrows
 * (provider, tools, task group, strings). Destroying the agent
 * simply frees the struct; the caller is responsible for releasing
 * those dependencies in the right order (sessions first, then the
 * agent, then the provider / tools / task group / http client it
 * held).
 */

#include "agent_private.h"
#include "session_private.h"

#include <stdlib.h>

xAiAgent xAiAgentCreate(const xAiAgentConf *conf) {
  if (!conf || !conf->loop || !conf->provider) return NULL;

  /* n_tools > 0 implies a non-NULL tools array. Catch this early so
   * session.c never has to guard against it. */
  if (conf->n_tools > 0 && !conf->tools) return NULL;

  struct xAiAgent_ *a = (struct xAiAgent_ *)calloc(1, sizeof(*a));
  if (!a) return NULL;

  a->loop           = conf->loop;
  a->provider       = conf->provider;
  a->model          = conf->model;
  a->system_prompt  = conf->system_prompt;
  a->tools          = conf->tools;
  a->n_tools        = conf->n_tools;
  a->task_group     = conf->task_group;
  a->max_turns      = conf->max_turns;
  a->max_tokens     = conf->max_tokens;

  /* Create the agent's built-in default session if the caller
   * provided a configuration template. The default session lives
   * for the agent's entire lifetime and is destroyed automatically
   * in xAiAgentDestroy. It is the user's primary conversation
   * entry — the origin field is honoured as-is (zero-initialised
   * configs default to xAiInputOrigin_User). */
  a->default_session = NULL;
  if (conf->default_session_conf) {
    xAiSession sess = xAiAgentCreateSession((xAiAgent)a,
                                             conf->default_session_conf);
    if (sess) {
      a->default_session = (struct xAiSession_ *)sess;
    }
    /* If default-session creation fails the agent is still usable;
     * xAiAgentDefaultSession will simply return NULL. */
  }

  return (xAiAgent)a;
}

void xAiAgentDestroy(xAiAgent agent) {
  if (!agent) return;
  struct xAiAgent_ *a = (struct xAiAgent_ *)agent;

  /* Tear down the built-in default session first. All other
   * user-created sessions must have been destroyed already by
   * the caller; this one we own. */
  if (a->default_session) {
    xAiSessionDestroy((xAiSession)a->default_session);
    a->default_session = NULL;
  }

  /* No other fields to release — the agent only borrows its
   * dependencies, and the caller is contractually required to
   * keep them alive until xAiAgentDestroy() is called. */
  free(agent);
}

/* ── Agent-layer hooks injected into Agent-created sessions ────────── */

/**
 * @brief Agent's L1 extraction hook.
 *
 * Fires in sess_fwd_on_done after produced entries have been
 * merged into history but before the caller's on_done. The agent
 * receives the full produced list and usage so it can extract
 * L1 memory candidates (structured observations from the
 * conversation output).
 *
 * @param sess        The session.
 * @param produced    Array of produced message entries (still alive;
 *                    owned by the Query's xArray).
 * @param n_produced  Number of entries in @p produced.
 * @param usage       Cumulative token usage for this run (may be NULL).
 * @param ud          The agent itself (passed as on_produced_ud).
 */
static void agent_on_produced(xAiSession                    sess,
                              const struct xAiSessionMsg_  *produced,
                              size_t                        n_produced,
                              const xAiUsage               *usage,
                              void                         *ud) {
  /* L1 memory extraction is not yet implemented. This hook is
   * wired in now so xAiAgentCreateSession callers can start
   * relying on it firing; a future commit will add the actual
   * extraction logic here. */
  (void)sess;
  (void)produced;
  (void)n_produced;
  (void)usage;
  (void)ud;
}

/**
 * @brief Agent's late-teardown hook.
 *
 * Fires during xAiSessionDestroy, while the session is still fully
 * live and its history is intact. Gives the agent a final chance
 * to digest the session for L1 memory, mood delta, analytics, etc.
 *
 * @param sess   The session about to be torn down.
 * @param owner  The agent itself (passed as finalizing_owner).
 */
static void agent_on_finalizing(xAiSession sess, void *owner) {
  /* L1 finalization is not yet implemented. Same as on_produced —
   * the hook is wired now so the API is in place. */
  (void)sess;
  (void)owner;
}

/* ── xAiAgentCreateSession ─────────────────────────────────────────── */

xAiSession xAiAgentCreateSession(xAiAgent             agent,
                                 const xAiSessionConf *conf) {
  if (!agent || !conf) return NULL;

  /* 1. Create the session normally via xAiSessionCreate. */
  xAiSession sess = xAiSessionCreate(agent, conf);
  if (!sess) return NULL;

  /* 2. Inject the agent's on_produced (L1 extraction hook).
   *    The session's internal copy of cbs is already made by
   *    xAiSessionCreate, so we patch the session struct directly. */
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  s->on_produced    = agent_on_produced;
  s->on_produced_ud = agent;  /* the agent itself */

  /* 3. Inject on_finalizing if the agent needs it. */
  s->on_finalizing     = agent_on_finalizing;
  s->finalizing_owner  = agent;

  return sess;
}

/* ── xAiAgentDefaultSession ────────────────────────────────────────── */

xAiSession xAiAgentDefaultSession(xAiAgent agent) {
  if (!agent) return NULL;
  return (xAiSession)((struct xAiAgent_ *)agent)->default_session;
}
