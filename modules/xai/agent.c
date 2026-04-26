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
#include <string.h>

/* ── xAiAgentCreate ────────────────────────────────────────────────── */

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

/* ── xAiAgentDestroy ───────────────────────────────────────────────── */

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

/* ── xAiAgentCreateSession ─────────────────────────────────────────── */

xAiSession xAiAgentCreateSession(xAiAgent             agent,
                                 const xAiSessionConf *conf) {
  if (!agent || !conf) return NULL;

  /* Create the session normally via xAiSessionCreate. */
  xAiSession sess = xAiSessionCreate(agent, conf);
  return sess;
}

/* ── xAiAgentDefaultSession ────────────────────────────────────────── */

xAiSession xAiAgentDefaultSession(xAiAgent agent) {
  if (!agent) return NULL;
  return (xAiSession)((struct xAiAgent_ *)agent)->default_session;
}
