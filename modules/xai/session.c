/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session.c - xAiSession implementation (MVP: text-only rounds)
 *
 * Scope of this file today:
 *   - Lifecycle (create / destroy / cancel).
 *   - Single-flight admission with xErrno_Busy.
 *   - History ownership: the session duplicates every text byte it
 *     receives from the caller (xAiSessionInput) and every chunk it
 *     receives from the provider, so callers can drop their message
 *     as soon as xAiSessionInput returns.
 *   - One streaming round per xAiSessionInput: build a
 *     xAiProviderSubmitConf view over the history, submit, stream
 *     text deltas, surface on_done.
 *
 * Intentionally NOT implemented yet (tracked in modules/xai/TODO.md):
 *   - Tool loop (tool_use → handler → tool_result → next round).
 *     If the provider emits a tool_call, the session reports
 *     xAiDoneReason_ToolError with an explanatory on_error so it is
 *     obvious from the outside that the MVP does not yet close the
 *     loop.
 *   - Local context_budget enforcement before submitting.
 *   - max_turns enforcement (MVP only issues one provider submit
 *     per xAiSessionInput; there is no multi-round loop yet because
 *     tools are the only thing that would extend a turn).
 *   - Prompt compression / snipping.
 */

#include "session_private.h"

#include "agent_private.h"
#include "provider_private.h" /* for ai_provider_submit / ai_provider_cancel */

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

static void msg_free(struct xAiSessionMsg_ *m) {
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

/* Append one text-only message to the history, copying every byte. */
static xErrno history_append_text(struct xAiSession_ *s, xAiRole role,
                                  const char *text, size_t len) {
  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
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

/* Append an incoming xAiMessage into our history. Only text content
 * blocks are persisted in the MVP; other block kinds (which the
 * caller really shouldn't be feeding in as user input anyway) are
 * ignored. */
static xErrno history_append_user_msg(struct xAiSession_ *s, xAiMessage msg) {
  /* Concatenate every text block into one string — the wire model
   * for an OpenAI "user" message is a single content string. */
  size_t total = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAiContentType_Text) {
      total += msg.contents[i].u.text.len;
    }
  }
  if (total == 0) {
    /* Empty message — still record role so history round-trip tests
     * see it, but no text buffer. */
    return history_append_text(s, msg.role, NULL, 0);
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
  slot->text     = buf;
  slot->text_len = total;
  return xErrno_Ok;
}

/* Append one byte range to the current-round assistant accumulator. */
static xErrno assist_append(struct xAiSession_ *s, const char *chunk,
                            size_t len) {
  if (len == 0) return xErrno_Ok;
  if (s->assist_len + len + 1 > s->assist_cap) {
    size_t new_cap = s->assist_cap ? s->assist_cap : 256;
    while (new_cap < s->assist_len + len + 1) new_cap *= 2;
    char *nb = (char *)realloc(s->assist_buf, new_cap);
    if (!nb) return xErrno_NoMemory;
    s->assist_buf = nb;
    s->assist_cap = new_cap;
  }
  memcpy(s->assist_buf + s->assist_len, chunk, len);
  s->assist_len += len;
  s->assist_buf[s->assist_len] = '\0';
  return xErrno_Ok;
}

static void assist_reset(struct xAiSession_ *s) {
  s->assist_len = 0;
  if (s->assist_buf) s->assist_buf[0] = '\0';
}

/* Build the transient (messages[], contents[]) view that submit()
 * expects. Arrays are caller-owned and freed after submit returns
 * (the provider treats them as borrowed for the lifetime of the
 * call — provider_openai_test confirmed this via BodyCaptured). */
struct view_ {
  xAiMessage *msgs;
  xAiContent *blocks; /* one per msg (MVP only supports text) */
  size_t      n_msgs;
};

static void view_free(struct view_ *v) {
  free(v->msgs);
  free(v->blocks);
  memset(v, 0, sizeof(*v));
}

static xErrno view_build(struct xAiSession_ *s, struct view_ *out) {
  memset(out, 0, sizeof(*out));
  size_t extra  = (s->system_prompt && s->system_prompt[0]) ? 1 : 0;
  size_t n_msgs = s->n_history + extra;
  if (n_msgs == 0) return xErrno_InvalidArg;

  out->msgs = (xAiMessage *)calloc(n_msgs, sizeof(xAiMessage));
  out->blocks = (xAiContent *)calloc(n_msgs, sizeof(xAiContent));
  if (!out->msgs || !out->blocks) {
    view_free(out);
    return xErrno_NoMemory;
  }
  out->n_msgs = n_msgs;

  size_t w = 0;
  if (extra) {
    out->blocks[w].type         = xAiContentType_Text;
    out->blocks[w].u.text.text  = s->system_prompt;
    out->blocks[w].u.text.len   = strlen(s->system_prompt);
    out->msgs[w].role           = xAiRole_System;
    out->msgs[w].contents       = &out->blocks[w];
    out->msgs[w].n              = 1;
    w++;
  }
  for (size_t i = 0; i < s->n_history; i++, w++) {
    struct xAiSessionMsg_ *m = &s->history[i];
    out->msgs[w].role         = m->role;
    if (m->text) {
      out->blocks[w].type        = xAiContentType_Text;
      out->blocks[w].u.text.text = m->text;
      out->blocks[w].u.text.len  = m->text_len;
      out->msgs[w].contents      = &out->blocks[w];
      out->msgs[w].n             = 1;
    } else {
      out->msgs[w].contents = NULL;
      out->msgs[w].n        = 0;
    }
  }
  return xErrno_Ok;
}

/* ── Provider callbacks ─────────────────────────────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg) {
  struct xAiSession_ *s = (struct xAiSession_ *)arg;
  if (s->cancelled) return;

  /* Accumulate so we can commit the full assistant message to
   * history on on_done. If allocation fails, we swallow the byte
   * range in history (the user still sees it live via on_text) and
   * let the on_done path translate the failure. A more aggressive
   * option would be to abort the round here; leaving that to
   * Commit 4 where error taxonomy is revisited. */
  (void)assist_append(s, chunk, len);

  if (s->cbs.on_text) {
    s->cbs.on_text((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

static void on_provider_tool_call(const xAiContent *call, void *arg) {
  struct xAiSession_ *s = (struct xAiSession_ *)arg;
  (void)call;
  /* MVP: report (exactly once) that tools aren't wired up yet. We
   * rely on the provider's on_done to terminate; by the time it
   * fires s->cancelled will cause us to report ToolError. */
  if (!s->cancelled && s->cbs.on_error) {
    s->cbs.on_error((xAiSession)s, xErrno_NotSupported,
                    "tool_use received but tool loop is not implemented in "
                    "this build of xAiSession (see modules/xai/TODO.md)",
                    s->cbs.user_data);
  }
  s->cancelled = 1; /* marks the round as tool-aborted */
}

static xAiDoneReason translate_stop(xAiProviderStopReason r, int cancelled,
                                    int tool_abort) {
  if (tool_abort) return xAiDoneReason_ToolError;
  if (cancelled) return xAiDoneReason_Aborted;
  switch (r) {
    case xAiProviderStop_EndTurn:    return xAiDoneReason_Completed;
    case xAiProviderStop_ToolUse:    return xAiDoneReason_ToolError;
    case xAiProviderStop_MaxTokens:  return xAiDoneReason_Completed;
    case xAiProviderStop_StopSeq:    return xAiDoneReason_Stopped;
    case xAiProviderStop_PromptLong: return xAiDoneReason_PromptTooLong;
    case xAiProviderStop_Error:      return xAiDoneReason_ModelError;
    case xAiProviderStop_Cancelled:  return xAiDoneReason_Aborted;
  }
  return xAiDoneReason_ModelError;
}

static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             void *arg) {
  struct xAiSession_ *s = (struct xAiSession_ *)arg;

  /* Determine whether this round was aborted because a tool_use
   * arrived (MVP can't handle it). cancelled got set both by
   * xAiSessionCancel and by the tool_call bailout. Distinguish via
   * reason. */
  int user_cancel = (reason == xAiProviderStop_Cancelled);
  int tool_abort  = (reason == xAiProviderStop_ToolUse) ||
                   (s->cancelled && !user_cancel);

  /* Commit the accumulated assistant text into history as an
   * Assistant message — even if the run errored, any streamed
   * prefix is still legitimate model output. */
  if (s->assist_len > 0) {
    (void)history_append_text(s, xAiRole_Assistant, s->assist_buf,
                              s->assist_len);
  }
  assist_reset(s);

  /* Surface any transport error before on_done so the caller sees
   * the diagnostic in order. Mirrors "exactly one of on_done /
   * on_error" by treating on_error here as an auxiliary signal —
   * see TODO.md "Finalise on_done vs on_error split". */
  if (reason == xAiProviderStop_Error && err != xErrno_Ok && s->cbs.on_error) {
    s->cbs.on_error((xAiSession)s, err, NULL, s->cbs.user_data);
  }

  xAiDoneReason coarse = translate_stop(reason, user_cancel, tool_abort);

  /* Clear in-flight state BEFORE the callback: a well-behaved
   * caller may re-enter xAiSessionInput from inside on_done, and
   * that must succeed (admission must already see running == 0). */
  s->running   = 0;
  s->cancelled = 0;

  if (s->cbs.on_done) {
    s->cbs.on_done((xAiSession)s, coarse, s->cbs.user_data);
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

  s->system_prompt  = conf->system_prompt ? conf->system_prompt : a->system_prompt;
  s->model          = conf->model ? conf->model : a->model;
  s->max_turns      = conf->max_turns > 0 ? conf->max_turns : a->max_turns;
  s->max_tokens     = conf->max_tokens > 0 ? conf->max_tokens : a->max_tokens;
  s->context_budget = conf->context_budget > 0 ? conf->context_budget
                                               : a->context_budget;

  return (xAiSession)s;
}

xErrno xAiSessionInput(xAiSession sess, xAiMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  struct xAiAgent_ *a   = (struct xAiAgent_ *)s->agent;

  if (s->running) return xErrno_Busy;

  /* Admission: append to history first. If anything goes wrong
   * from here on we can pop the tail to roll back. */
  size_t history_checkpoint = s->n_history;
  xErrno rc = history_append_user_msg(s, msg);
  if (rc != xErrno_Ok) return rc;

  /* Build the submit view over the current history. */
  struct view_ v;
  rc = view_build(s, &v);
  if (rc != xErrno_Ok) {
    /* Roll back the user message we just appended. */
    while (s->n_history > history_checkpoint) {
      msg_free(&s->history[--s->n_history]);
    }
    return rc;
  }

  xAiProviderSubmitConf pc = {0};
  pc.model                 = s->model;
  pc.messages              = v.msgs;
  pc.n_messages            = v.n_msgs;
  pc.tools                 = (const xAiTool **)a->tools; /* borrowed */
  pc.n_tools               = a->n_tools;
  pc.temperature           = -1; /* not set */
  pc.max_tokens            = s->max_tokens;
  pc.stop                  = NULL;

  xAiProviderStreamCallbacks cbs = {0};
  cbs.on_text                    = on_provider_text;
  cbs.on_tool_call               = on_provider_tool_call;
  cbs.on_done                    = on_provider_done;

  /* Flip running BEFORE submit — submit may invoke callbacks
   * synchronously on error paths, and our on_done clears running. */
  s->running   = 1;
  s->cancelled = 0;
  assist_reset(s);

  rc = ai_provider_submit(a->provider, &pc, &cbs, s);

  /* The provider is allowed to borrow v.msgs/v.blocks only for the
   * duration of submit(). provider_openai copies fields it needs
   * into its HTTP body before returning. */
  view_free(&v);

  if (rc != xErrno_Ok) {
    /* Submit rejected. Tear the round down without firing on_done —
     * this is the "failed to start" branch, and the caller receives
     * the error through our return value. */
    s->running = 0;
    while (s->n_history > history_checkpoint) {
      msg_free(&s->history[--s->n_history]);
    }
    return rc;
  }

  return xErrno_Ok;
}

void xAiSessionCancel(xAiSession sess) {
  if (!sess) return;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  if (!s->running) return;

  s->cancelled = 1;

  struct xAiAgent_ *a = (struct xAiAgent_ *)s->agent;
  ai_provider_cancel(a->provider);
  /* on_done will arrive through the provider (reason=Cancelled) and
   * our translate_stop() will surface xAiDoneReason_Aborted. */
}

void xAiSessionDestroy(xAiSession sess) {
  if (!sess) return;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  /* If a run is still in flight, cancel it. The provider is
   * required to deliver on_done synchronously or before submit()
   * returns a rejection. We don't attempt to pump the loop here;
   * session.h warns callers to quiesce the session first, and
   * TODO.md tracks proper async teardown. */
  if (s->running) {
    xAiSessionCancel(sess);
  }

  for (size_t i = 0; i < s->n_history; i++) msg_free(&s->history[i]);
  free(s->history);
  free(s->assist_buf);
  free(s);
}
