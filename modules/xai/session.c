/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session.c - xAiSession implementation
 *
 * Scope:
 *   - Lifecycle (create / destroy / cancel).
 *   - Single-flight admission with xErrno_Busy.
 *   - History ownership: every byte (user text, assistant text,
 *     tool_use arguments, tool_result output) is duplicated into
 *     session-owned storage. Callers can drop their xAiMessage as
 *     soon as xAiSessionInput returns.
 *   - Query lifecycle: each accepted xAiSessionInput creates a fresh
 *     Query via xAiQueryCreate, runs it with xAiQueryRun, and
 *     destroys it from a forwarding on_done once the terminal
 *     callback has fired.
 *
 * The provider / tool loop itself lives in query.c; the Session
 * installs a static set of forwarding callbacks that re-dispatch
 * Query-level events (on_text, on_thinking, on_done, ...) to the
 * caller's xAiSessionCallbacks with the Session handle.
 *
 * Intentionally still deferred (see modules/xai/TODO.md):
 *   - Parallel tool dispatch via xTaskGroup when concurrent_safe is
 *     set. Today every handler runs synchronously on the loop thread.
 *   - User-confirmation gate for needs_confirm tools.
 *   - Local context_budget compression. The session still forwards
 *     the provider's PromptLong signal as xAiDoneReason_PromptTooLong.
 *   - Proper async teardown when destroy is called mid-flight.
 */

#include "session_private.h"

#include "agent_private.h"

#include <xai/message.h>
#include <xai/provider.h>
#include <xai/query.h>
#include <xai/session.h>
#include <xbase/base.h>
#include <xbase/error.h>

#include <stdlib.h>
#include <string.h>

/* ── Small helpers ──────────────────────────────────────────────────── */

static char *dup_bytes(const char *src, size_t len) {
  if (!src) return NULL;
  char *out = (char *)malloc(len + 1);
  if (!out) return NULL;
  if (len) memcpy(out, src, len);
  out[len] = '\0';
  return out;
}

static char *dup_cstr(const char *src) {
  if (!src) return NULL;
  return dup_bytes(src, strlen(src));
}

void ai_session_msg_free(struct xAiSessionMsg_ *m) {
  if (!m) return;
  free(m->text);
  free(m->tool_use_id);
  free(m->tool_use_name);
  free(m->tool_use_args);
  free(m->tool_result_id);
  free(m->tool_result_output);
  memset(m, 0, sizeof(*m));
}

/* Grow history by one slot, zero-initialised. Returns the slot or
 * NULL on allocation failure. */
static struct xAiSessionMsg_ *history_grow(struct xAiSession_ *s) {
  if (s->n_history + 1 > s->cap_history) {
    size_t new_cap = s->cap_history ? s->cap_history * 2 : 8;
    struct xAiSessionMsg_ *nh = (struct xAiSessionMsg_ *)realloc(
        s->history, new_cap * sizeof(*nh));
    if (!nh) return NULL;
    s->history     = nh;
    s->cap_history = new_cap;
  }
  struct xAiSessionMsg_ *slot = &s->history[s->n_history++];
  memset(slot, 0, sizeof(*slot));
  return slot;
}

/* ── History append API (shared with query.c via session_private.h) ── */

xErrno ai_history_append_text(struct xAiSession_ *s, xAiRole role,
                              const char *text, size_t len) {
  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
  slot->kind = xAiSessionEntry_Text;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      s->n_history--;
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

xErrno ai_history_append_tool_use(struct xAiSession_ *s, const char *id,
                                  const char *name, const char *args) {
  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Assistant;
  slot->kind = xAiSessionEntry_ToolUse;
  slot->tool_use_id   = dup_cstr(id ? id : "");
  slot->tool_use_name = dup_cstr(name ? name : "");
  slot->tool_use_args = dup_cstr(args ? args : "{}");
  if (!slot->tool_use_id || !slot->tool_use_name || !slot->tool_use_args) {
    ai_session_msg_free(slot);
    s->n_history--;
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Payload reuses the Text-kind @c text / @c text_len slot so
 * ai_session_msg_free() stays uniform. */
xErrno ai_history_append_thinking(struct xAiSession_ *s, const char *text,
                                  size_t len) {
  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Assistant;
  slot->kind = xAiSessionEntry_Thinking;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      s->n_history--;
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

xErrno ai_history_append_tool_result(struct xAiSession_ *s, const char *id,
                                     const char *output, size_t output_len,
                                     int is_error) {
  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Tool;
  slot->kind = xAiSessionEntry_ToolResult;
  slot->tool_result_id = dup_cstr(id ? id : "");
  if (output_len > 0) {
    slot->tool_result_output = dup_bytes(output, output_len);
    slot->tool_result_output_len = output_len;
  } else if (output) {
    slot->tool_result_output     = dup_cstr(output);
    slot->tool_result_output_len = strlen(output);
  } else {
    slot->tool_result_output     = dup_cstr("");
    slot->tool_result_output_len = 0;
  }
  slot->tool_result_is_error = is_error;
  if (!slot->tool_result_id || !slot->tool_result_output) {
    ai_session_msg_free(slot);
    s->n_history--;
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Append an incoming xAiMessage (caller-supplied, shallow view) into
 * our history. Every text block is concatenated into a single text
 * entry; non-text blocks on the user side are ignored. Exposed to
 * query.c via session_private.h so xAiQueryRun can perform the
 * Phase-α history append on its input. Phase β moves the history
 * append back into session.c once Queries take explicit message
 * arrays. */
xErrno ai_history_append_user_msg(struct xAiSession_ *s, xAiMessage msg) {
  size_t total = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAiContentType_Text) {
      total += msg.contents[i].u.text.len;
    }
  }
  if (total == 0) {
    return ai_history_append_text(s, msg.role, NULL, 0);
  }

  char *buf = (char *)malloc(total + 1);
  if (!buf) return xErrno_NoMemory;
  size_t off = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAiContentType_Text) {
      size_t n = msg.contents[i].u.text.len;
      if (n) memcpy(buf + off, msg.contents[i].u.text.text, n);
      off += n;
    }
  }
  buf[total] = '\0';

  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) {
    free(buf);
    return xErrno_NoMemory;
  }
  slot->role     = msg.role;
  slot->kind     = xAiSessionEntry_Text;
  slot->text     = buf;
  slot->text_len = total;
  return xErrno_Ok;
}

/* ── Query → Session callback forwarding ──────────────────────────────
 *
 * The Query fires its own @c q->cbs.* stream with a Query handle.
 * Sessions install the shims below so every event is re-dispatched
 * to the caller's xAiSessionCallbacks with the Session handle and
 * the caller's user_data. The Query callbacks carry the owning
 * Session as user_data.
 */

static void sess_fwd_on_text(xAiQuery q, const char *chunk, size_t len,
                             void *ud) {
  (void)q;
  struct xAiSession_ *s = (struct xAiSession_ *)ud;
  if (s->cbs.on_text) {
    s->cbs.on_text((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

static void sess_fwd_on_thinking(xAiQuery q, const char *chunk, size_t len,
                                 void *ud) {
  (void)q;
  struct xAiSession_ *s = (struct xAiSession_ *)ud;
  if (s->cbs.on_thinking) {
    s->cbs.on_thinking((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

static void sess_fwd_on_error(xAiQuery q, xErrno err, const char *msg,
                              void *ud) {
  (void)q;
  struct xAiSession_ *s = (struct xAiSession_ *)ud;
  if (s->cbs.on_error) {
    s->cbs.on_error((xAiSession)s, err, msg, s->cbs.user_data);
  }
}

static void sess_fwd_on_tool(xAiQuery q, const char *tool_name, int started,
                             void *ud) {
  (void)q;
  struct xAiSession_ *s = (struct xAiSession_ *)ud;
  if (s->cbs.on_tool) {
    s->cbs.on_tool((xAiSession)s, tool_name, started, s->cbs.user_data);
  }
}

/* Terminal forwarding: fire the caller's on_done first, then release
 * the Query. Destroy-in-on_done is safe because query.c's finalize
 * path does not touch @p q after firing our callback (see
 * query_finalize in query.c).
 *
 * Order note: caller's on_done runs while @c s->query still points
 * at @p q, so xAiSessionQuery / xAiQueryUsage observed from inside
 * on_done still work. Only after the callback returns do we detach
 * and free (xAiQueryDestroy nulls s->query for us). */
static void sess_fwd_on_done(xAiQuery q, xAiDoneReason reason,
                             const xAiUsage *usage, void *ud) {
  struct xAiSession_ *s = (struct xAiSession_ *)ud;
  if (s->cbs.on_done) {
    s->cbs.on_done((xAiSession)s, reason, usage, s->cbs.user_data);
  }
  xAiQueryDestroy(q);
}

/* One static callback set — stamped into every Query the Session
 * spawns, with user_data re-bound per-create to the Session handle. */
static const xAiQueryCallbacks SESSION_FWD_CBS = {
  .on_text     = sess_fwd_on_text,
  .on_thinking = sess_fwd_on_thinking,
  .on_done     = sess_fwd_on_done,
  .on_error    = sess_fwd_on_error,
  .on_tool     = sess_fwd_on_tool,
  .user_data   = NULL, /* filled in per-Query */
};

/* ── Public API ─────────────────────────────────────────────────────── */

xAiSession xAiSessionCreate(xAiAgent agent, const xAiSessionConf *conf) {
  if (!agent || !conf) return NULL;

  struct xAiSession_ *s = (struct xAiSession_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  struct xAiAgent_ *a = (struct xAiAgent_ *)agent;

  s->agent = agent;
  s->cbs   = conf->cbs;

  s->system_prompt  = conf->system_prompt ? conf->system_prompt
                                          : a->system_prompt;
  s->model          = conf->model ? conf->model : a->model;
  s->max_turns      = conf->max_turns > 0 ? conf->max_turns : a->max_turns;
  s->max_tokens     = conf->max_tokens > 0 ? conf->max_tokens : a->max_tokens;
  s->context_budget = conf->context_budget > 0 ? conf->context_budget
                                               : a->context_budget;

  /* Session-lifetime properties: stamped here, never mutated. Zero
   * for @c origin collapses to xAiInputOrigin_User, which is also
   * the conservative default for callers who simply calloc the
   * conf. */
  s->origin           = conf->origin;
  s->on_finalizing    = conf->on_finalizing;
  s->finalizing_owner = conf->finalizing_owner;

  /* @c s->query starts NULL (from calloc). A Query is allocated on
   * demand by xAiSessionInput and released from sess_fwd_on_done. */

  return (xAiSession)s;
}

xErrno xAiSessionInput(xAiSession sess, xAiMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  /* Single-flight: one live Query per Session. If the previous run
   * is still in flight refuse with Busy. */
  if (s->query) return xErrno_Busy;

  /* Spawn a fresh Query with Session-level forwarding shims bound
   * to this Session. xAiQueryCreate also sets s->query so a second
   * Input call during the same run hits the Busy branch above. */
  xAiQueryConf qc  = {0};
  qc.cbs           = SESSION_FWD_CBS;
  qc.cbs.user_data = s;

  xAiQuery q = xAiQueryCreate(sess, &qc);
  if (!q) return xErrno_NoMemory;

  /* xAiQueryRun performs the Phase-α history append and submits the
   * first round. On failure it rolls the history append back; we
   * just need to release the Query we just created. */
  xErrno rc = xAiQueryRun(q, msg);
  if (rc != xErrno_Ok) {
    xAiQueryDestroy(q); /* Clears s->query back to NULL. */
    return rc;
  }
  return xErrno_Ok;
}

void xAiSessionCancel(xAiSession sess) {
  /* Thin wrapper over the query-level entry point. Both exist so
   * callers can target whichever layer their code already tracks;
   * the underlying semantics are identical (cancel this session's
   * single in-flight run). */
  xAiQueryCancel(xAiSessionQuery(sess));
}

void xAiSessionDestroy(xAiSession sess) {
  if (!sess) return;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  /* Tear down the Query first (if any). xAiQueryDestroy will
   * cancel-then-free synchronously; in the common "natural
   * completion" path sess_fwd_on_done has already destroyed it, so
   * this branch is mostly a safety net for mid-flight destroys. */
  if (s->query) {
    xAiQueryDestroy((xAiQuery)s->query);
    /* xAiQueryDestroy nulls s->query for us. */
  }

  /* Fire the late-teardown hook while the session is still fully
   * live (history intact). Detach before calling so a misbehaving
   * hook that triggers a second destroy won't re-enter here. Per
   * contract the hook runs at most once. */
  if (s->on_finalizing) {
    xAiSessionFinalizingFn hook  = s->on_finalizing;
    void                  *owner = s->finalizing_owner;
    s->on_finalizing             = NULL;
    s->finalizing_owner          = NULL;
    hook(sess, owner);
  }

  for (size_t i = 0; i < s->n_history; i++) {
    ai_session_msg_free(&s->history[i]);
  }
  free(s->history);
  free(s);
}

xAiInputOrigin xAiSessionOrigin(xAiSession sess) {
  if (!sess) return xAiInputOrigin_User;
  return ((struct xAiSession_ *)sess)->origin;
}
