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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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
 * xAiAgentCreateSession and freed when the session is destroyed
 * (the callback resets the owner pointer, but we never free it
 * from the callback itself — the agent frees all owner structs
 * in xAiAgentDestroy via a simple linked list; for now we just
 * leak the small struct for the session lifetime since it's tiny).
 *
 * Actually, since the callback fires for the last time during
 * xAiSessionDestroy (Finalizing), we can free it there. But to
 * keep things simple and safe, we let the agent struct outlive
 * all sessions (by contract), so the borrowed agent pointer is
 * always valid. */
struct agent_l1_ctx_ {
  struct xAiAgent_ *agent;      /* borrowed, always valid by contract */
  char             *session_id; /* owned copy, for file path building */
};

/* Write a single JSONL line for one xAiSessionMsg.
 * Returns the number of bytes written, or -1 on error. */
static int write_msg_jsonl_(FILE *fp, const xAiSessionMsg *m) {
  /* role */
  const char *role_str = "system";
  switch (m->role) {
  case xAiRole_System:
    role_str = "system";
    break;
  case xAiRole_User:
    role_str = "user";
    break;
  case xAiRole_Assistant:
    role_str = "assistant";
    break;
  case xAiRole_Tool:
    role_str = "tool";
    break;
  }

  /* kind */
  const char *kind_str = "text";
  switch (m->kind) {
  case xAiSessionEntryKind_Text:
    kind_str = "text";
    break;
  case xAiSessionEntryKind_ToolUse:
    kind_str = "tool_use";
    break;
  case xAiSessionEntryKind_ToolResult:
    kind_str = "tool_result";
    break;
  case xAiSessionEntryKind_Thinking:
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

  if (m->kind == xAiSessionEntryKind_Text ||
      m->kind == xAiSessionEntryKind_Thinking) {
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
  } else if (m->kind == xAiSessionEntryKind_ToolUse) {
    if (m->tool_use_id)
      n += fprintf(fp, ",\"tool_use_id\":\"%s\"", m->tool_use_id);
    if (m->tool_use_name)
      n += fprintf(fp, ",\"tool_use_name\":\"%s\"", m->tool_use_name);
    if (m->tool_use_args)
      n += fprintf(fp, ",\"tool_use_args\":%s", m->tool_use_args);
  } else if (m->kind == xAiSessionEntryKind_ToolResult) {
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
static void agent_l1_preserve_cb_(xAiSession sess, const xAiSessionMsg *msgs,
                                  size_t n_msgs, xAiL1PreserveReason reason,
                                  void *owner) {
  (void)sess;
  if (!owner) return;

  struct agent_l1_ctx_ *ctx = (struct agent_l1_ctx_ *)owner;
  struct xAiAgent_     *a   = ctx->agent;

  /* On Finalizing we must always free the owner context, even if
   * there is nothing to persist — otherwise the strdup'd session_id
   * and the ctx struct itself would leak. */
  if (!msgs || n_msgs == 0) {
    if (reason == xAiL1PreserveReason_Finalizing) {
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
  if (reason == xAiL1PreserveReason_Finalizing) {
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

/* ── xAiAgentCreate ────────────────────────────────────────────────── */

xAiAgent xAiAgentCreate(const xAiAgentConf *conf) {
  if (!conf || !conf->loop || !conf->provider) return NULL;

  /* n_tools > 0 implies a non-NULL tools array. Catch this early so
   * session.c never has to guard against it. */
  if (conf->n_tools > 0 && !conf->tools) return NULL;

  struct xAiAgent_ *a = (struct xAiAgent_ *)calloc(1, sizeof(*a));
  if (!a) return NULL;

  a->loop          = conf->loop;
  a->provider      = conf->provider;
  a->model         = conf->model;
  a->system_prompt = conf->system_prompt;
  a->tools         = conf->tools;
  a->n_tools       = conf->n_tools;
  a->task_group    = conf->task_group;
  a->max_turns     = conf->max_turns;
  a->max_tokens    = conf->max_tokens;
  a->agent_id      = conf->agent_id ? conf->agent_id : "default";
  a->data_dir      = conf->data_dir ? conf->data_dir : "/tmp/xai";
  a->session_seq   = 0;

  /* Create the agent's built-in default session if the caller
   * provided a configuration template. The default session lives
   * for the agent's entire lifetime and is destroyed automatically
   * in xAiAgentDestroy. It is the user's primary conversation
   * entry — the origin field is honoured as-is (zero-initialised
   * configs default to xAiInputOrigin_User). */
  a->default_session = NULL;
  if (conf->default_session_conf) {
    /* Force the default session's id to "default" so its L1
     * persistence path is deterministic:
     *   {data_dir}/agents/{agent_id}/sessions/default/memory.jsonl
     * rather than a random id like "s_0001ab". */
    xAiSessionConf dsconf = *conf->default_session_conf;
    dsconf.session_id     = "default";

    xAiSession sess =
      xAiAgentCreateSession((xAiAgent)a, &dsconf);
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

/* ── xAiAgentId ────────────────────────────────────────────────────── */

const char *xAiAgentId(xAiAgent agent) {
  if (!agent) return NULL;
  return ((struct xAiAgent_ *)agent)->agent_id;
}

/* ── xAiAgentCreateSession ─────────────────────────────────────────── */

xAiSession xAiAgentCreateSession(xAiAgent agent, const xAiSessionConf *conf) {
  if (!agent || !conf) return NULL;

  struct xAiAgent_ *a = (struct xAiAgent_ *)agent;

  /* Prepare a session configuration that may differ from the
   * caller's original: we may need to inject session_id and
   * the L1 preserve callback. */
  xAiSessionConf effective = *conf;

  /* Auto-generate a session_id if the caller didn't supply one
   * and the agent has agent_id configured (meaning L1 persistence
   * is active). */
  char generated_id[12] = {0};
  if (!effective.session_id && a->agent_id && a->data_dir) {
    a->session_seq++;
    gen_session_id_(generated_id, a->session_seq);
    effective.session_id = generated_id;
  }

  /* If the agent has both agent_id and data_dir configured and
   * the caller hasn't already wired an L1 preserve callback,
   * inject our own. */
  struct agent_l1_ctx_ *l1_ctx = NULL;
  if (a->agent_id && a->data_dir && !effective.on_l1_preserve) {
    l1_ctx = (struct agent_l1_ctx_ *)calloc(1, sizeof(*l1_ctx));
    if (l1_ctx) {
      l1_ctx->agent = a;
      l1_ctx->session_id =
        effective.session_id ? strdup(effective.session_id) : NULL;
      effective.on_l1_preserve    = agent_l1_preserve_cb_;
      effective.l1_preserve_owner = l1_ctx;
    }
    /* If calloc fails we simply skip L1 wiring — the session
     * works fine without persistence. */
  }

  /* Create the session normally via xAiSessionCreate. */
  xAiSession sess = xAiSessionCreate(agent, &effective);

  if (!sess && l1_ctx) {
    /* Session creation failed — clean up the L1 context. */
    free(l1_ctx->session_id);
    free(l1_ctx);
  }

  return sess;
}

/* ── xAiAgentDefaultSession ────────────────────────────────────────── */

xAiSession xAiAgentDefaultSession(xAiAgent agent) {
  if (!agent) return NULL;
  return (xAiSession)((struct xAiAgent_ *)agent)->default_session;
}
