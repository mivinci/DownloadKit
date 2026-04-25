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
 *   - Query lifecycle: each accepted xAiSessionInput builds the
 *     complete message list the model should see (System prompt +
 *     rolling history + the new user turn), creates a fresh Query
 *     via xAiQueryCreate, runs it with xAiQueryRun, and destroys
 *     it from a forwarding on_done once the terminal callback has
 *     fired. Anything the Query produced during the run is pulled
 *     out via ai_query_take_produced() and merged back into the
 *     Session's history before the Query is released.
 *
 * The provider / tool loop itself lives in query.c; the Session
 * installs a static set of forwarding callbacks that re-dispatch
 * Query-level events (on_text, on_thinking, on_done, ...) to the
 * caller's xAiSessionCallbacks with the Session handle. The Query
 * runs off an explicit message array and never reads or writes
 * s->history directly — that is the boundary this split enforces.
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

/* Concatenate every text block of an incoming user-role xAiMessage
 * into a single text entry and append it to history. Non-text blocks
 * are ignored (user-side tool_result entries come in via the tool
 * pipeline, not xAiSessionInput). Kept static: only xAiSessionInput
 * consumes this shape now that the Query no longer drives history
 * writes itself. */
static xErrno history_append_user_msg(struct xAiSession_ *s, xAiMessage msg) {
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

/* ── Building the Query input from Session state ────────────────────
 *
 * Every xAiSessionInput run hands the Query a complete, self-
 * contained message array: (optional) System prompt + the rolling
 * history the Session already has, including the user message we
 * just appended. Each xAiMessage borrows from session-owned storage
 * (history entries live until the run terminates or the Session is
 * torn down) — the Query still deep-copies, so once xAiQueryRun
 * returns this transient array can go away.
 */

struct sess_input_view_ {
  xAiMessage *msgs;
  xAiContent *blocks;
  size_t      n_msgs;
  size_t      n_blocks;
};

static void sess_input_view_free(struct sess_input_view_ *v) {
  free(v->msgs);
  free(v->blocks);
  memset(v, 0, sizeof(*v));
}

/* Build a message array from the current session state. Consecutive
 * Assistant entries are folded into one xAiMessage (so thinking +
 * text + tool_use blocks travel together); other roles map 1:1. */
static xErrno sess_input_view_build(struct xAiSession_      *s,
                                    struct sess_input_view_ *out) {
  memset(out, 0, sizeof(*out));
  size_t extra_system = (s->system_prompt && s->system_prompt[0]) ? 1 : 0;

  /* Pass 1: count. */
  size_t n_msgs   = extra_system;
  size_t n_blocks = extra_system;
  for (size_t i = 0; i < s->n_history;) {
    if (s->history[i].role == xAiRole_Assistant) {
      size_t j = i;
      while (j < s->n_history && s->history[j].role == xAiRole_Assistant) j++;
      n_msgs += 1;
      n_blocks += (j - i);
      i = j;
    } else {
      n_msgs += 1;
      n_blocks += 1;
      i += 1;
    }
  }
  if (n_msgs == 0) return xErrno_InvalidArg;

  out->msgs   = (xAiMessage *)calloc(n_msgs, sizeof(xAiMessage));
  out->blocks = (xAiContent *)calloc(n_blocks, sizeof(xAiContent));
  if (!out->msgs || !out->blocks) {
    sess_input_view_free(out);
    return xErrno_NoMemory;
  }
  out->n_msgs   = n_msgs;
  out->n_blocks = n_blocks;

  /* Pass 2: populate. */
  size_t mi = 0;
  size_t bi = 0;

  if (extra_system) {
    out->blocks[bi].type        = xAiContentType_Text;
    out->blocks[bi].u.text.text = s->system_prompt;
    out->blocks[bi].u.text.len  = strlen(s->system_prompt);
    out->msgs[mi].role          = xAiRole_System;
    out->msgs[mi].contents      = &out->blocks[bi];
    out->msgs[mi].n             = 1;
    mi++;
    bi++;
  }

  for (size_t i = 0; i < s->n_history;) {
    struct xAiSessionMsg_ *m = &s->history[i];
    if (m->role == xAiRole_Assistant) {
      size_t block_start = bi;
      size_t j           = i;
      while (j < s->n_history && s->history[j].role == xAiRole_Assistant) {
        struct xAiSessionMsg_ *mm = &s->history[j];
        xAiContent            *b  = &out->blocks[bi++];
        if (mm->kind == xAiSessionEntry_Text) {
          b->type        = xAiContentType_Text;
          b->u.text.text = mm->text ? mm->text : "";
          b->u.text.len  = mm->text_len;
        } else if (mm->kind == xAiSessionEntry_Thinking) {
          b->type            = xAiContentType_Thinking;
          b->u.thinking.text = mm->text ? mm->text : "";
          b->u.thinking.len  = mm->text_len;
        } else if (mm->kind == xAiSessionEntry_ToolUse) {
          b->type                 = xAiContentType_ToolUse;
          b->u.tool_use.id        = mm->tool_use_id;
          b->u.tool_use.name      = mm->tool_use_name;
          b->u.tool_use.args_json = mm->tool_use_args;
        }
        j++;
      }
      out->msgs[mi].role     = xAiRole_Assistant;
      out->msgs[mi].contents = &out->blocks[block_start];
      out->msgs[mi].n        = bi - block_start;
      mi++;
      i = j;
    } else {
      xAiContent *b = &out->blocks[bi];
      if (m->kind == xAiSessionEntry_ToolResult) {
        b->type                     = xAiContentType_ToolResult;
        b->u.tool_result.id         = m->tool_result_id;
        b->u.tool_result.output     = m->tool_result_output;
        b->u.tool_result.output_len = m->tool_result_output_len;
        b->u.tool_result.is_error   = m->tool_result_is_error;
      } else {
        b->type        = xAiContentType_Text;
        b->u.text.text = m->text ? m->text : "";
        b->u.text.len  = m->text_len;
      }
      out->msgs[mi].role     = m->role;
      out->msgs[mi].contents = b;
      out->msgs[mi].n        = 1;
      mi++;
      bi++;
      i++;
    }
  }

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

/* Terminal forwarding: pull the Query's produced-turn list into the
 * Session's history first (so the on_done handler sees the updated
 * conversation), then fire the caller's on_done, then release the
 * Query. Destroy-in-on_done is safe because query.c's finalize path
 * does not touch @p q after firing our callback.
 *
 * Order note: caller's on_done runs while @c s->query still points
 * at @p q, so xAiSessionQuery / xAiQueryUsage observed from inside
 * on_done still work. Only after the callback returns do we detach
 * and free (xAiQueryDestroy nulls s->query for us).
 *
 * Produced merging happens on every terminal reason, including
 * Aborted / MaxTurns / errors: whatever partial output the model
 * managed to emit before the run ended is legitimate conversation
 * history and the next xAiSessionInput should see it. */
static void sess_fwd_on_done(xAiQuery q, xAiDoneReason reason,
                             const xAiUsage *usage, void *ud) {
  struct xAiSession_ *s = (struct xAiSession_ *)ud;

  /* Steal the produced list (ownership transfer). After this the
   * Query's own destructor won't touch these entries. */
  struct xAiSessionMsg_ *produced   = NULL;
  size_t                 n_produced = 0;
  ai_query_take_produced((struct xAiQuery_ *)q, &produced, &n_produced);

  /* Append every produced entry into history by bitwise move: the
   * struct owns its strings, and history_grow hands us a freshly
   * zeroed slot, so we can just copy the struct across. On growth
   * failure the remainder (and whatever we haven't moved yet) is
   * released via ai_session_msg_free + free(produced) below. */
  for (size_t i = 0; i < n_produced; i++) {
    struct xAiSessionMsg_ *slot = history_grow(s);
    if (!slot) {
      /* OOM merging: release the rest of the produced list. What
       * we've already committed stays in history. */
      for (size_t j = i; j < n_produced; j++) {
        ai_session_msg_free(&produced[j]);
      }
      break;
    }
    *slot = produced[i];
    memset(&produced[i], 0, sizeof(produced[i])); /* moved */
  }
  free(produced);

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

  /* Commit the user message to history first so the input view
   * below includes it. If the Query submit later fails we'll roll
   * this back. */
  size_t history_checkpoint = s->n_history;
  xErrno rc                 = history_append_user_msg(s, msg);
  if (rc != xErrno_Ok) return rc;

  /* Build the complete message array the Query should run on
   * (system prompt + rolling history including the new user turn). */
  struct sess_input_view_ view;
  rc = sess_input_view_build(s, &view);
  if (rc != xErrno_Ok) {
    /* Roll back the user append — nothing observable happened. */
    while (s->n_history > history_checkpoint) {
      ai_session_msg_free(&s->history[--s->n_history]);
    }
    return rc;
  }

  /* Spawn a fresh Query with Session-level forwarding shims bound
   * to this Session. xAiQueryCreate also sets s->query so a second
   * Input call during the same run hits the Busy branch above. */
  xAiQueryConf qc  = {0};
  qc.cbs           = SESSION_FWD_CBS;
  qc.cbs.user_data = s;

  xAiQuery q = xAiQueryCreate(sess, &qc);
  if (!q) {
    sess_input_view_free(&view);
    while (s->n_history > history_checkpoint) {
      ai_session_msg_free(&s->history[--s->n_history]);
    }
    return xErrno_NoMemory;
  }

  /* Hand the input array to the Query. xAiQueryRun deep-copies
   * everything, so we can release @c view immediately afterwards
   * regardless of success/failure. */
  rc = xAiQueryRun(q, view.msgs, view.n_msgs);
  sess_input_view_free(&view);

  if (rc != xErrno_Ok) {
    xAiQueryDestroy(q); /* Clears s->query back to NULL. */
    /* No on_done fires on the failure path — roll back the user
     * message too so history reflects what actually happened. */
    while (s->n_history > history_checkpoint) {
      ai_session_msg_free(&s->history[--s->n_history]);
    }
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
