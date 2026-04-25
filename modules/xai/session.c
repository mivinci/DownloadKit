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
 *   - Run finalisation: snapshot usage + reset the embedded Query +
 *     fire the caller's on_done.
 *
 * The provider / tool loop itself lives in query.c; xAiSessionInput
 * arms the Query and hands off via ai_query_submit.
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
#include "provider_private.h" /* ai_provider_cancel                        */

#include <xai/message.h>
#include <xai/provider.h>
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
 * entry; non-text blocks on the user side are ignored. Kept static:
 * only xAiSessionInput in this TU consumes user-shaped xAiMessage. */
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

/* ── Run finalisation ──────────────────────────────────────────────── */

void ai_session_finish_run(struct xAiSession_ *s, xAiDoneReason reason) {
  /* Snapshot usage before reset: the callback sees the running
   * totals we accumulated across every provider round, or NULL if
   * nothing ever reported. */
  xAiUsage usage_snapshot = s->query.usage;
  int      had_usage      = s->query.saw_usage;

  /* Hand the Query back to its idle shape (buffers kept for reuse,
   * flags cleared, usage accumulator zeroed). */
  ai_query_reset_round(&s->query);

  if (s->cbs.on_done) {
    s->cbs.on_done((xAiSession)s, reason,
                   had_usage ? &usage_snapshot : NULL,
                   s->cbs.user_data);
  }
}

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

  /* Leave the Query in its zero-initialised idle shape: ai_query_arm
   * at xAiSessionInput time will set session back-pointer, reset
   * the usage accumulator to all-(-1), and flip running=1. */

  return (xAiSession)s;
}

xErrno xAiSessionInput(xAiSession sess, xAiMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  if (s->query.running) return xErrno_Busy;

  size_t history_checkpoint = s->n_history;
  xErrno rc = history_append_user_msg(s, msg);
  if (rc != xErrno_Ok) return rc;

  /* Arm BEFORE submit: submit may deliver callbacks synchronously,
   * and on_provider_done clears running via ai_session_finish_run. */
  ai_query_arm(&s->query, s);

  rc = ai_query_submit(&s->query);
  if (rc != xErrno_Ok) {
    /* Round never started — don't fire on_done. Undo the user
     * message we appended so the history tracks what actually
     * happened. */
    s->query.running = 0;
    s->query.turn    = 0;
    while (s->n_history > history_checkpoint) {
      ai_session_msg_free(&s->history[--s->n_history]);
    }
    return rc;
  }

  return xErrno_Ok;
}

void xAiSessionCancel(xAiSession sess) {
  if (!sess) return;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  if (!s->query.running) return;

  ai_query_cancel_mark(&s->query);

  struct xAiAgent_ *a = (struct xAiAgent_ *)s->agent;
  ai_provider_cancel(a->provider);
  /* on_provider_done will arrive with reason=Cancelled (or we are
   * between rounds and dispatch_pending_tools will notice). */
}

void xAiSessionDestroy(xAiSession sess) {
  if (!sess) return;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  if (s->query.running) {
    xAiSessionCancel(sess);
  }

  for (size_t i = 0; i < s->n_history; i++) {
    ai_session_msg_free(&s->history[i]);
  }
  free(s->history);
  ai_query_dispose(&s->query);
  free(s);
}
