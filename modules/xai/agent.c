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
  a->context_budget = conf->context_budget;

  return (xAiAgent)a;
}

void xAiAgentDestroy(xAiAgent agent) {
  if (!agent) return;
  /* No fields to release — the agent only borrows its dependencies,
   * and the caller is contractually required to keep them alive
   * until every session derived from this agent has been destroyed
   * and then xAiAgentDestroy() is called. Matches xAiProviderDestroy()
   * in provider.c. */
  free(agent);
}
