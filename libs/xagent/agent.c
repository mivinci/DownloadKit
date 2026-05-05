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
 * (provider, tools, task group, strings). Destroying the agent
 * simply frees the struct; the caller is responsible for releasing
 * those dependencies in the right order (sessions first, then the
 * agent, then the provider / tools / task group / http client it
 * held).
 */

#include "agent_private.h"
#include "session_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <xagent/memory.h>

/* ── L1 memory persistence ───────────────────────────────────────────
 *
 * When the agent has both agent_id and data_dir configured, it
 * auto-wires an L1 preserve callback into every session it creates.
 * The callback appends session history entries as JSONL lines to:
 *
 *   {data_dir}/agents/{agent_id}/sessions/{session_id}/memory.jsonl
 *
 * The file is opened in append mode for every batch, so each
 * invocation (Truncated, Compacted, Finalizing) appends
 * independently. The directory tree is created on first use.
 */

/* Owner context passed via l1_preserve_owner. Allocated by
 * xAgentCreateSession and freed when the session is destroyed
 * (the callback resets the owner pointer, but we never free it
 * from the callback itself — the agent frees all owner structs
 * in xAgentDestroy via a simple linked list; for now we just
 * leak the small struct for the session lifetime since it's tiny).
 *
 * Actually, since the callback fires for the last time during
 * xAgentSessionDestroy (Finalizing), we can free it there. But to
 * keep things simple and safe, we let the agent struct outlive
 * all sessions (by contract), so the borrowed agent pointer is
 * always valid. */
struct agent_l1_ctx_ {
  struct xAgent_ *agent;      /* borrowed, always valid by contract */
  char             *session_id; /* owned copy, for file path building */
};

/* Write a single JSONL line for one xAgentSessionMsg.
 * Returns the number of bytes written, or -1 on error. */
static int write_msg_jsonl_(FILE *fp, const xAgentSessionMsg *m) {
  /* role */
  const char *role_str = "system";
  switch (m->role) {
  case xAgentRole_System:
    role_str = "system";
    break;
  case xAgentRole_User:
    role_str = "user";
    break;
  case xAgentRole_Assistant:
    role_str = "assistant";
    break;
  case xAgentRole_Tool:
    role_str = "tool";
    break;
  }

  /* kind */
  const char *kind_str = "text";
  switch (m->kind) {
  case xAgentSessionEntryKind_Text:
    kind_str = "text";
    break;
  case xAgentSessionEntryKind_ToolUse:
    kind_str = "tool_use";
    break;
  case xAgentSessionEntryKind_ToolResult:
    kind_str = "tool_result";
    break;
  case xAgentSessionEntryKind_Thinking:
    kind_str = "thinking";
    break;
  }

  /* We write a minimal JSON object per entry. Text fields are
   * naively escaped (replace " with \" and \ with \\). This is
   * not a full JSON serializer but sufficient for L1 storage. */

  /* For now, use a simple approach: write role, kind, and the
   * relevant payload fields. We skip NULL/empty strings. */
  int n = 0;
  n += fprintf(fp, "{\"role\":\"%s\",\"kind\":\"%s\"", role_str, kind_str);

  if (m->kind == xAgentSessionEntryKind_Text ||
      m->kind == xAgentSessionEntryKind_Thinking) {
    if (m->text && m->text_len > 0) {
      n += fprintf(fp, ",\"text\":\"");
      /* Write text with basic JSON escaping */
      for (size_t i = 0; i < m->text_len; i++) {
        char c = m->text[i];
        if (c == '"')
          n += fprintf(fp, "\\\"");
        else if (c == '\\')
          n += fprintf(fp, "\\\\");
        else if (c == '\n')
          n += fprintf(fp, "\\n");
        else if (c == '\r')
          n += fprintf(fp, "\\r");
        else if (c == '\t')
          n += fprintf(fp, "\\t");
        else if ((unsigned char)c < 0x20)
          n += fprintf(fp, "\\u%04x", (unsigned char)c);
        else
          n += fprintf(fp, "%c", c);
      }
      n += fprintf(fp, "\"");
    }
  } else if (m->kind == xAgentSessionEntryKind_ToolUse) {
    if (m->tool_use_id)
      n += fprintf(fp, ",\"tool_use_id\":\"%s\"", m->tool_use_id);
    if (m->tool_use_name)
      n += fprintf(fp, ",\"tool_use_name\":\"%s\"", m->tool_use_name);
    if (m->tool_use_args)
      n += fprintf(fp, ",\"tool_use_args\":%s", m->tool_use_args);
  } else if (m->kind == xAgentSessionEntryKind_ToolResult) {
    if (m->tool_result_id)
      n += fprintf(fp, ",\"tool_result_id\":\"%s\"", m->tool_result_id);
    if (m->tool_result_is_error) n += fprintf(fp, ",\"is_error\":true");
    if (m->tool_result_output && m->tool_result_output_len > 0) {
      n += fprintf(fp, ",\"output\":\"");
      for (size_t i = 0; i < m->tool_result_output_len; i++) {
        char c = m->tool_result_output[i];
        if (c == '"')
          n += fprintf(fp, "\\\"");
        else if (c == '\\')
          n += fprintf(fp, "\\\\");
        else if (c == '\n')
          n += fprintf(fp, "\\n");
        else if (c == '\r')
          n += fprintf(fp, "\\r");
        else if (c == '\t')
          n += fprintf(fp, "\\t");
        else if ((unsigned char)c < 0x20)
          n += fprintf(fp, "\\u%04x", (unsigned char)c);
        else
          n += fprintf(fp, "%c", c);
      }
      n += fprintf(fp, "\"");
    }
  }

  n += fprintf(fp, "}\n");
  return n;
}

/* Recursively create directories for a file path (like mkdir -p).
 * path must be a NUL-terminated string. Trailing filename is
 * handled by finding the last '/' and creating everything up to it. */
static int mkdirs_for_file_(const char *path) {
  if (!path) return -1;

  /* Find the last separator — everything before it is the directory. */
  const char *last_sep = strrchr(path, '/');
  if (!last_sep) return 0; /* no directory component */

  size_t dir_len = (size_t)(last_sep - path);
  if (dir_len == 0) return 0; /* root directory */

  char *dir = (char *)malloc(dir_len + 1);
  if (!dir) return -1;
  memcpy(dir, path, dir_len);
  dir[dir_len] = '\0';

  /* Try to create each component, ignoring EEXIST. */
  for (size_t i = 1; i <= dir_len; i++) {
    if (dir[i] == '/' || dir[i] == '\0') {
      char saved = dir[i];
      dir[i]     = '\0';
      if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        free(dir);
        return -1;
      }
      dir[i] = saved;
    }
  }

  free(dir);
  return 0;
}

/* The agent-level L1 preserve callback. */
static void agent_l1_preserve_cb_(xAgentSession sess, const xAgentSessionMsg *msgs,
                                  size_t n_msgs, xAgentL1PreserveReason reason,
                                  void *owner) {
  (void)sess;
  if (!owner) return;

  struct agent_l1_ctx_ *ctx = (struct agent_l1_ctx_ *)owner;
  struct xAgent_     *a   = ctx->agent;

  /* On Finalizing we must always free the owner context, even if
   * there is nothing to persist — otherwise the strdup'd session_id
   * and the ctx struct itself would leak. */
  if (!msgs || n_msgs == 0) {
    if (reason == xAgentL1PreserveReason_Finalizing) {
      free(ctx->session_id);
      free(ctx);
    }
    return;
  }

  if (!a->data_dir || !a->agent_id || !ctx->session_id) return;

  /* Build the file path:
   *   {data_dir}/agents/{agent_id}/sessions/{session_id}/memory.jsonl */
  size_t path_len = strlen(a->data_dir) + strlen("/agents/") +
                    strlen(a->agent_id) + strlen("/sessions/") +
                    strlen(ctx->session_id) + strlen("/memory.jsonl") + 1;
  char *path = (char *)malloc(path_len);
  if (!path) return;

  snprintf(path, path_len, "%s/agents/%s/sessions/%s/memory.jsonl", a->data_dir,
           a->agent_id, ctx->session_id);

  /* Ensure the directory exists. */
  if (mkdirs_for_file_(path) != 0) {
    free(path);
    return;
  }

  /* Open in append mode and write each entry as a JSONL line. */
  FILE *fp = fopen(path, "a");
  if (!fp) {
    free(path);
    return;
  }

  for (size_t i = 0; i < n_msgs; i++) {
    write_msg_jsonl_(fp, &msgs[i]);
  }

  fclose(fp);
  free(path);

  /* Finalizing is the last invocation — free the owner context
   * since the session will never call us again. The session
   * sets l1_preserve_owner = NULL after this returns, so there
   * is no dangling pointer risk. */
  if (reason == xAgentL1PreserveReason_Finalizing) {
    free(ctx->session_id);
    free(ctx);
  }
}

/* ── xAgentMemory-backed preserve callback ───────────────────────────
 *
 * When the agent is created with an explicit xAgentMemory store,
 * we bypass the built-in JSONL writer entirely and route every
 * preserve batch through xAgentMemoryAppend. The reason enum on
 * xAgentL1PreserveReason maps 1:1 to xAgentMemoryAppendReason's
 * first three values (Truncated=0, Compacted=1, Finalizing=2).
 */
static void agent_memory_preserve_cb_(xAgentSession sess,
                                      const xAgentSessionMsg *msgs,
                                      size_t n_msgs,
                                      xAgentL1PreserveReason reason,
                                      void *owner) {
  (void)sess;
  if (!owner) return;

  struct agent_l1_ctx_ *ctx = (struct agent_l1_ctx_ *)owner;
  struct xAgent_       *a   = ctx->agent;

  /* As with the JSONL path, on Finalizing we free the ctx even
   * when the batch is empty so we don't leak the session_id copy. */
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
    q.agent_id   = a->agent_id;
    q.session_id = ctx->session_id;
    /* Direct mapping of L1 reason → memory append reason. */
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

  a->loop           = conf->loop;
  a->provider       = resolved_provider;
  a->model          = resolved_model;
  a->model_registry = conf->model_registry; /* NULL on legacy path */
  a->system_prompt  = conf->system_prompt;
  a->tools          = conf->tools;
  a->tools_count    = conf->tools_count;
  a->task_group     = conf->task_group;
  a->max_turns      = conf->max_turns;
  a->max_tokens     = conf->max_tokens;
  a->agent_id             = conf->agent_id ? conf->agent_id : "default";
  a->data_dir             = conf->data_dir ? conf->data_dir : "/tmp/xagent";
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
    /* Force the default session's id to "default" so its L1
     * persistence path is deterministic:
     *   {data_dir}/agents/{agent_id}/sessions/default/memory.jsonl
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

/* ── xAgentId ────────────────────────────────────────────────────── */

const char *xAgentId(xAgent agent) {
  if (!agent) return NULL;
  return ((struct xAgent_ *)agent)->agent_id;
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
   * and the agent has agent_id configured (meaning L1 persistence
   * is active). */
  char generated_id[12] = {0};
  if (!effective.session_id && a->agent_id &&
      (a->memory || a->data_dir)) {
    a->session_seq++;
    gen_session_id_(generated_id, a->session_seq);
    effective.session_id = generated_id;
  }

  /* Decide which preserve backend to wire:
   *   - If the caller already set on_l1_preserve, honour it.
   *   - Else if the agent was given an explicit xAgentMemory
   *     store, route through xAgentMemoryAppend.
   *   - Else fall back to the built-in JSONL writer when
   *     agent_id + data_dir are both configured.
   *   - Else leave on_l1_preserve unset (no persistence). */
  struct agent_l1_ctx_ *l1_ctx = NULL;
  if (!effective.on_l1_preserve && a->agent_id) {
    xAgentSessionL1PreserveFunc chosen = NULL;
    if (a->memory) {
      chosen = agent_memory_preserve_cb_;
    } else if (a->data_dir) {
      chosen = agent_l1_preserve_cb_;
    }
    if (chosen) {
      l1_ctx = (struct agent_l1_ctx_ *)calloc(1, sizeof(*l1_ctx));
      if (l1_ctx) {
        l1_ctx->agent = a;
        l1_ctx->session_id =
          effective.session_id ? strdup(effective.session_id) : NULL;
        effective.on_l1_preserve    = chosen;
        effective.l1_preserve_owner = l1_ctx;
      }
      /* If calloc fails we simply skip L1 wiring — the session
       * works fine without persistence. */
    }
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
   * When the agent has an explicit memory store, we pull whatever
   * the store has for (agent_id, session_id) and push the entries
   * onto the fresh session's history_arr so the NEXT xAgentSessionInput
   * submits to the provider with the full prior context. This is
   * what makes sessions feel "continued" rather than cold-started
   * when a caller reuses a stable session_id across process runs.
   *
   * Caveat (B1): this commit does NOT yet de-duplicate against
   * subsequent L1 preserves, so when the session is torn down the
   * Finalizing batch will re-append these primed entries to the
   * store. Retrieve-only-the-tail semantics in the built-in JSONL
   * backend mean this is harmless for correctness — the newest
   * view is always the authoritative one — but the on-disk file
   * will grow. A follow-up commit will thread a persisted_prefix
   * index through the session so preserves can skip the already-
   * stored region. */
  if (sess && a->memory && effective.session_id) {
    struct xAgentSession_ *s = (struct xAgentSession_ *)sess;

    xAgentMemoryQuery rq;
    memset(&rq, 0, sizeof(rq));
    rq.agent_id   = a->agent_id;
    rq.session_id = effective.session_id;
    /* No budget / recency hints — B1 primes the whole tail the
     * backend is willing to hand back. The session's own budget
     * gate will clip it later if the combined token count
     * overflows. */

    xAgentMemoryHits hits;
    memset(&hits, 0, sizeof(hits));
    if (xAgentMemoryRetrieve(a->memory, &rq, &hits) == xErrno_Ok &&
        hits.n_entries > 0) {
      for (size_t i = 0; i < hits.n_entries; i++) {
        const xAgentSessionMsg *m = &hits.entries[i];
        /* Dispatch on kind and copy through the session's normal
         * append helpers so memory ownership and the release
         * callback (ai_session_msg_free) stay uniform with the
         * rest of the history. Errors are swallowed here — a
         * failed prime must not prevent the caller from using the
         * session, it just means the primed row is missing. */
        switch (m->kind) {
        case xAgentSessionEntryKind_Text:
          ai_history_append_text(s, m->role, m->text, m->text_len);
          break;
        case xAgentSessionEntryKind_Thinking:
          ai_history_append_thinking(s, m->text, m->text_len);
          break;
        case xAgentSessionEntryKind_ToolUse:
          ai_history_append_tool_use(s, m->tool_use_id, m->tool_use_name,
                                     m->tool_use_args);
          break;
        case xAgentSessionEntryKind_ToolResult:
          ai_history_append_tool_result(s, m->tool_result_id,
                                        m->tool_result_output,
                                        m->tool_result_output_len,
                                        m->tool_result_is_error);
          break;
        }
      }
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
