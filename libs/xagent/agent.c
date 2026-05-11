/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent.c - Capability-template bookkeeping for the xai core
 *
 * An agent is pure state: every field in xAgentConf is copied into
 * struct xAgent_ and never mutated thereafter. No event loop state,
 * no provider calls, no tool dispatch — that all lives in session.c.
 *
 * The caller retains ownership of everything the agent borrows
 * (provider, tools, task group, strings, memory store). Destroying
 * the agent simply frees the struct; the caller is responsible for
 * releasing those dependencies in the right order (sessions first,
 * then the agent, then the provider / tools / task group / http
 * client / memory store it held).
 */

#include "agent_private.h"
#include "session_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <xagent/memory.h>

/* ── L1 memory persistence ───────────────────────────────────────────
 *
 * When the agent is configured with an xAgentMemory store, it
 * auto-wires an L1 preserve callback into every session it creates.
 * The callback routes each persist batch (Truncated / Compacted /
 * Finalizing) through xAgentMemoryAppend(), letting the store decide
 * where and how to persist. Without a memory store configured the
 * agent does nothing special — sessions run in-memory only.
 *
 * Owner context carries the session_id across calls because the
 * session struct is torn down before the final Finalizing batch
 * fires (the callback itself is responsible for freeing the ctx
 * at Finalizing time).
 */
struct agent_l1_ctx_ {
  struct xAgent_ *agent;      /* borrowed, always valid by contract */
  char           *session_id; /* owned copy, survives session teardown */
};

/* The agent's memory-backed L1 preserve callback.
 *
 * L1 reason values map 1:1 to xAgentMemoryAppendReason's first
 * three entries (Truncated=0, Compacted=1, Finalizing=2), so we
 * pass the enum through by cast. */
static void agent_memory_preserve_cb_(xAgentSession sess,
                                      const xAgentSessionMsg *msgs,
                                      size_t n_msgs,
                                      xAgentL1PreserveReason reason,
                                      void *owner) {
  (void)sess;
  if (!owner) return;

  struct agent_l1_ctx_ *ctx = (struct agent_l1_ctx_ *)owner;
  struct xAgent_       *a   = ctx->agent;

  /* On Finalizing we must always free the owner context, even when
   * the batch is empty, so we don't leak the session_id copy. */
  if (!msgs || n_msgs == 0) {
    if (reason == xAgentL1PreserveReason_Finalizing) {
      free(ctx->session_id);
      free(ctx);
    }
    return;
  }

  if (a->memory && ctx->session_id) {
    xAgentMemoryQuery q;
    memset(&q, 0, sizeof(q));
    q.session_id = ctx->session_id;
    xAgentMemoryAppend(a->memory, &q, (xAgentMemoryAppendReason)reason, msgs,
                       n_msgs);
  }

  if (reason == xAgentL1PreserveReason_Finalizing) {
    free(ctx->session_id);
    free(ctx);
  }
}

/* ── Simple random session ID generator ──────────────────────────────
 *
 * Generates a short random string like "s_1a3b5c7d". Uses
 * rand() seeded once per process — good enough for unique
 * session IDs within a single agent; a proper UUID can replace
 * this later.
 */

static int rand_seeded_ = 0;

static void seed_rand_once_(void) {
  if (!rand_seeded_) {
    srand((unsigned int)time(NULL) ^ (unsigned int)clock());
    rand_seeded_ = 1;
  }
}

/* Generate a session ID into a caller-supplied buffer.
 * Format: "s_{8hex}". buf must hold at least 12 bytes. */
static void gen_session_id_(char *buf, uint64_t seq) {
  seed_rand_once_();
  unsigned int r = (unsigned int)rand();
  snprintf(buf, 12, "s_%04x%02x", (unsigned int)(seq & 0xFFFF), r & 0xFF);
}

/* ── xAgentCreate ────────────────────────────────────────────────── */

xAgent xAgentCreate(const xAgentConf *conf) {
  if (!conf || !conf->loop) return NULL;

  /* Resolve the provider+model from either the legacy single-provider
   * path or the new registry path. Exactly one must be used. */
  xAgentProvider resolved_provider = NULL;
  const char    *resolved_model    = NULL;

  int legacy   = (conf->provider != NULL);
  int registry = (conf->model_registry != NULL);
  if (legacy == registry) {
    /* Both set, or neither set — ambiguous. */
    return NULL;
  }

  if (legacy) {
    resolved_provider = conf->provider;
    resolved_model    = conf->model; /* may be NULL */
  } else {
    if (!conf->default_model_id || !*conf->default_model_id) return NULL;
    const xAgentModelSpec *spec =
      xAgentModelRegistryGet(conf->model_registry, conf->default_model_id);
    if (!spec) return NULL;
    resolved_provider = spec->provider;
    resolved_model    = spec->model; /* may be NULL */
  }

  /* tools_count > 0 implies a non-NULL tools array. Catch this early so
   * session.c never has to guard against it. */
  if (conf->tools_count > 0 && !conf->tools) return NULL;

  struct xAgent_ *a = (struct xAgent_ *)calloc(1, sizeof(*a));
  if (!a) return NULL;

  a->loop                 = conf->loop;
  a->provider             = resolved_provider;
  a->model                = resolved_model;
  a->model_registry       = conf->model_registry; /* NULL on legacy path */
  a->system_prompt        = conf->system_prompt;
  a->tools                = conf->tools;
  a->tools_count          = conf->tools_count;
  a->task_group           = conf->task_group;
  a->max_turns            = conf->max_turns;
  a->max_tokens           = conf->max_tokens;
  a->memory               = conf->memory;
  a->enable_sidecar_query = conf->enable_sidecar_query;
  a->session_seq          = 0;

  /* Create the agent's built-in default session if the caller
   * provided a configuration template. The default session lives
   * for the agent's entire lifetime and is destroyed automatically
   * in xAgentDestroy. It is the user's primary conversation
   * entry — the origin field is honoured as-is (zero-initialised
   * configs default to xAgentInputOrigin_User). */
  a->default_session = NULL;
  if (conf->default_session_conf) {
    /* Force the default session's id to "default" so its memory
     * slot is deterministic:
     *   {root_dir}/sessions/default/history.jsonl
     * rather than a random id like "s_0001ab". */
    xAgentSessionConf dsconf = *conf->default_session_conf;
    dsconf.session_id     = "default";

    xAgentSession sess =
      xAgentCreateSession((xAgent)a, &dsconf);
    if (sess) {
      a->default_session = (struct xAgentSession_ *)sess;
    }
    /* If default-session creation fails the agent is still usable;
     * xAgentDefaultSession will simply return NULL. */
  }

  return (xAgent)a;
}

/* ── xAgentDestroy ───────────────────────────────────────────────── */

void xAgentDestroy(xAgent agent) {
  if (!agent) return;
  struct xAgent_ *a = (struct xAgent_ *)agent;

  /* Tear down the built-in default session first. All other
   * user-created sessions must have been destroyed already by
   * the caller; this one we own. */
  if (a->default_session) {
    xAgentSessionDestroy((xAgentSession)a->default_session);
    a->default_session = NULL;
  }

  /* No other fields to release — the agent only borrows its
   * dependencies, and the caller is contractually required to
   * keep them alive until xAgentDestroy() is called. */
  free(agent);
}

/* ── xAgentCreateSession ─────────────────────────────────────────── */

xAgentSession xAgentCreateSession(xAgent agent, const xAgentSessionConf *conf) {
  if (!agent || !conf) return NULL;

  struct xAgent_ *a = (struct xAgent_ *)agent;

  /* Prepare a session configuration that may differ from the
   * caller's original: we may need to inject session_id and
   * the L1 preserve callback. */
  xAgentSessionConf effective = *conf;

  /* Auto-generate a session_id if the caller didn't supply one
   * and we have a memory store (so persistence is active and the
   * id matters for routing). */
  char generated_id[12] = {0};
  if (!effective.session_id && a->memory) {
    a->session_seq++;
    gen_session_id_(generated_id, a->session_seq);
    effective.session_id = generated_id;
  }

  /* Wire the agent's memory-backed L1 preserve callback when the
   * caller hasn't supplied one of their own and we actually have
   * a memory store to route to. Caller-supplied hooks always win
   * so higher layers can intercept / augment persistence. */
  struct agent_l1_ctx_ *l1_ctx = NULL;
  if (!effective.on_l1_preserve && a->memory) {
    l1_ctx = (struct agent_l1_ctx_ *)calloc(1, sizeof(*l1_ctx));
    if (l1_ctx) {
      l1_ctx->agent = a;
      l1_ctx->session_id =
        effective.session_id ? strdup(effective.session_id) : NULL;
      effective.on_l1_preserve    = agent_memory_preserve_cb_;
      effective.l1_preserve_owner = l1_ctx;
    }
    /* If calloc fails we simply skip L1 wiring — the session
     * works fine without persistence. */
  }

  /* Create the session normally via xAgentSessionCreate. */
  xAgentSession sess = xAgentSessionCreate(agent, &effective);

  if (!sess && l1_ctx) {
    /* Session creation failed — clean up the L1 context. */
    free(l1_ctx->session_id);
    free(l1_ctx);
  }

  /* ── Memory prime: replay persisted history into this session ──
   *
   * When the agent has a memory store, we pull whatever the store
   * has for this session_id and push the entries onto the fresh
   * session's history_arr so the NEXT xAgentSessionInput submits to
   * the provider with the full prior context. This is what makes
   * sessions feel "continued" rather than cold-started when a
   * caller reuses a stable session_id across process runs.
   *
   * persisted_prefix is bumped past the primed region so the
   * Finalizing batch on teardown won't re-append these entries to
   * the store and inflate the on-disk file. */
  if (sess && a->memory && effective.session_id) {
    struct xAgentSession_ *s = (struct xAgentSession_ *)sess;

    xAgentMemoryQuery rq;
    memset(&rq, 0, sizeof(rq));
    rq.session_id = effective.session_id;
    /* No budget / recency hints — prime the whole tail the backend
     * is willing to hand back. The session's own budget gate will
     * clip it later if the combined token count overflows. */

    xAgentMemoryHits hits;
    memset(&hits, 0, sizeof(hits));
    if (xAgentMemoryRetrieve(a->memory, &rq, &hits) == xErrno_Ok &&
        hits.n_entries > 0) {
      size_t primed = 0;
      for (size_t i = 0; i < hits.n_entries; i++) {
        const xAgentSessionMsg *m = &hits.entries[i];
        xErrno prc = xErrno_Ok;
        /* Dispatch on kind and copy through the session's normal
         * append helpers so memory ownership and the release
         * callback (ai_session_msg_free) stay uniform with the
         * rest of the history. Errors short-circuit the loop so
         * we stop counting primed rows at the first failure —
         * otherwise a partial prime would leave persisted_prefix
         * ahead of what's actually in history_arr. */
        switch (m->kind) {
        case xAgentSessionEntryKind_Text:
          prc = ai_history_append_text(s, m->role, m->text, m->text_len,
                                       m->is_summary);
          break;
        case xAgentSessionEntryKind_Thinking:
          prc = ai_history_append_thinking(s, m->text, m->text_len);
          break;
        case xAgentSessionEntryKind_ToolUse:
          prc = ai_history_append_tool_use(s, m->tool_use_id,
                                           m->tool_use_name,
                                           m->tool_use_args);
          break;
        case xAgentSessionEntryKind_ToolResult:
          prc = ai_history_append_tool_result(s, m->tool_result_id,
                                              m->tool_result_output,
                                              m->tool_result_output_len,
                                              m->tool_result_is_error);
          break;
        }
        if (prc != xErrno_Ok) break;
        primed++;
      }
      /* Tell the session these leading entries are already in the
       * external store, so on-destroy / on-trim preserves won't
       * re-append them and inflate the on-disk file. */
      s->persisted_prefix = primed;
    }
    xAgentMemoryReleaseHits(a->memory, &hits);
  }

  return sess;
}

/* ── xAgentDefaultSession ────────────────────────────────────────── */

xAgentSession xAgentDefaultSession(xAgent agent) {
  if (!agent) return NULL;
  return (xAgentSession)((struct xAgent_ *)agent)->default_session;
}
