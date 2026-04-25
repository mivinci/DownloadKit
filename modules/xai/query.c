/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query.c - xAiQuery implementation
 *
 * One Query is one end-to-end run of the provider / tool loop:
 *
 *     arm → submit round 1 → (tools?) → submit round 2 → ... → finish
 *
 * Session owns the durable state (history, configuration, caller
 * callbacks) and embeds exactly one Query. Query owns the transient
 * state of a single run (streaming buffers, pending tool calls,
 * cumulative usage, running/cancelled flags).
 *
 * This file is intentionally unaware of xAiAgent layering: it reaches
 * back into its owning Session via @c q->session and leaves policy
 * like "when does a run start?" / "who destroys the Session?" to
 * session.c.
 */

#include "query_private.h"
#include "session_private.h"

#include "agent_private.h"
#include "provider_private.h" /* ai_provider_submit / ai_provider_cancel   */
#include "tool_private.h"     /* ai_tool_name / ai_tool_invoke             */

#include <xai/message.h>
#include <xai/provider.h>
#include <xai/session.h>
#include <xai/tool.h>
#include <xbase/base.h>
#include <xbase/error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Small duplicating helpers ──────────────────────────────────────
 *
 * query.c carries its own copy of dup_bytes / dup_cstr rather than
 * pulling them from session.c. They are 8 lines total, and keeping
 * them local is what lets query.c evolve toward a standalone unit
 * (see docs/todo/xai_architecture.md §10 Step 3). */

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

/* ── Pending tool-call bookkeeping ──────────────────────────────── */

static void pending_free_entry(struct xAiQueryPending_ *p) {
  if (!p) return;
  free(p->id);
  free(p->name);
  free(p->args_json);
  memset(p, 0, sizeof(*p));
}

static void pending_reset(struct xAiQuery_ *q) {
  for (size_t i = 0; i < q->n_pending; i++) {
    pending_free_entry(&q->pending[i]);
  }
  q->n_pending = 0;
}

/* Append one pending tool_call. Copies every string. */
static xErrno pending_append(struct xAiQuery_ *q, const xAiContent *call) {
  if (q->n_pending + 1 > q->cap_pending) {
    size_t new_cap = q->cap_pending ? q->cap_pending * 2 : 4;
    struct xAiQueryPending_ *np = (struct xAiQueryPending_ *)realloc(
        q->pending, new_cap * sizeof(*np));
    if (!np) return xErrno_NoMemory;
    q->pending     = np;
    q->cap_pending = new_cap;
  }
  struct xAiQueryPending_ *slot = &q->pending[q->n_pending];
  memset(slot, 0, sizeof(*slot));
  slot->id        = dup_cstr(call->u.tool_use.id ? call->u.tool_use.id : "");
  slot->name      = dup_cstr(call->u.tool_use.name ? call->u.tool_use.name
                                                   : "");
  slot->args_json = dup_cstr(call->u.tool_use.args_json
                                 ? call->u.tool_use.args_json
                                 : "{}");
  if (!slot->id || !slot->name || !slot->args_json) {
    pending_free_entry(slot);
    return xErrno_NoMemory;
  }
  q->n_pending++;
  return xErrno_Ok;
}

/* ── Round-scoped streaming accumulators ───────────────────────── */

/* Append one byte range to the current-round assistant text buffer. */
static xErrno assist_append(struct xAiQuery_ *q, const char *chunk, size_t len) {
  if (len == 0) return xErrno_Ok;
  if (q->assist_len + len + 1 > q->assist_cap) {
    size_t new_cap = q->assist_cap ? q->assist_cap : 256;
    while (new_cap < q->assist_len + len + 1) new_cap *= 2;
    char *nb = (char *)realloc(q->assist_buf, new_cap);
    if (!nb) return xErrno_NoMemory;
    q->assist_buf = nb;
    q->assist_cap = new_cap;
  }
  memcpy(q->assist_buf + q->assist_len, chunk, len);
  q->assist_len += len;
  q->assist_buf[q->assist_len] = '\0';
  return xErrno_Ok;
}

static void assist_reset(struct xAiQuery_ *q) {
  q->assist_len = 0;
  if (q->assist_buf) q->assist_buf[0] = '\0';
}

/* Same shape as assist_append, but for the current-round reasoning /
 * thinking stream. Kept separate so the two never race to the same
 * buffer and so view_build can emit them as distinct content blocks
 * on the wire. */
static xErrno reasoning_append(struct xAiQuery_ *q, const char *chunk,
                               size_t len) {
  if (len == 0) return xErrno_Ok;
  if (q->reasoning_len + len + 1 > q->reasoning_cap) {
    size_t new_cap = q->reasoning_cap ? q->reasoning_cap : 256;
    while (new_cap < q->reasoning_len + len + 1) new_cap *= 2;
    char *nb = (char *)realloc(q->reasoning_buf, new_cap);
    if (!nb) return xErrno_NoMemory;
    q->reasoning_buf = nb;
    q->reasoning_cap = new_cap;
  }
  memcpy(q->reasoning_buf + q->reasoning_len, chunk, len);
  q->reasoning_len += len;
  q->reasoning_buf[q->reasoning_len] = '\0';
  return xErrno_Ok;
}

static void reasoning_reset(struct xAiQuery_ *q) {
  q->reasoning_len = 0;
  if (q->reasoning_buf) q->reasoning_buf[0] = '\0';
}

/* ── Usage accumulator ─────────────────────────────────────────── */

/* Fold one round's usage into the running total for this run.
 *
 * Semantics: -1 is the "unknown" sentinel on both sides. If the
 * round contributes a real number for a given field and the running
 * total is still -1, we replace (first real report wins the initial
 * value). If both are real numbers, we add. If the round's field is
 * -1, we leave the running total alone. Anthropic-style
 * cache_creation / cache_read will slot in the same way once we
 * teach the provider to parse them. */
static void usage_accumulate(struct xAiQuery_ *q, const xAiUsage *round) {
  if (!round) return;
  q->saw_usage = 1;

#define XAI_FOLD(field)                                                       \
  do {                                                                        \
    if (round->field >= 0) {                                                  \
      q->usage.field = (q->usage.field < 0)                                   \
                           ? round->field                                     \
                           : q->usage.field + round->field;                   \
    }                                                                         \
  } while (0)

  XAI_FOLD(prompt_tokens);
  XAI_FOLD(completion_tokens);
  XAI_FOLD(total_tokens);

#undef XAI_FOLD
}

static void usage_reset(struct xAiQuery_ *q) {
  q->saw_usage               = 0;
  q->usage.prompt_tokens     = -1;
  q->usage.completion_tokens = -1;
  q->usage.total_tokens      = -1;
}

/* ── Submit-view construction ──────────────────────────────────── */

/* The transient arrays passed to provider_submit. The provider
 * borrows them only for the duration of submit(); we tear them down
 * immediately after. */
struct view_ {
  xAiMessage *msgs;   /* n_msgs entries                             */
  xAiContent *blocks; /* n_blocks entries, referenced by msgs[i]    */
  size_t      n_msgs;
  size_t      n_blocks;
};

static void view_free(struct view_ *v) {
  free(v->msgs);
  free(v->blocks);
  memset(v, 0, sizeof(*v));
}

/* Fold consecutive Assistant entries into one xAiMessage (possibly
 * carrying both text and tool_use blocks); every other entry maps
 * 1:1 to an xAiMessage.
 *
 * First pass counts output sizes, second pass populates them. */
static xErrno view_build(struct xAiQuery_ *q, struct view_ *out) {
  struct xAiSession_ *s = q->session;
  memset(out, 0, sizeof(*out));
  size_t extra_system = (s->system_prompt && s->system_prompt[0]) ? 1 : 0;

  /* Count messages and blocks. */
  size_t n_msgs   = extra_system;
  size_t n_blocks = extra_system; /* system prompt gets one text block */
  for (size_t i = 0; i < s->n_history; ) {
    if (s->history[i].role == xAiRole_Assistant) {
      /* Fold all consecutive Assistant entries into one msg. */
      size_t j = i;
      while (j < s->n_history && s->history[j].role == xAiRole_Assistant) j++;
      n_msgs   += 1;
      n_blocks += (j - i);
      i = j;
    } else {
      /* One block per user/tool entry. */
      n_msgs   += 1;
      n_blocks += 1;
      i += 1;
    }
  }

  if (n_msgs == 0) return xErrno_InvalidArg;

  out->msgs   = (xAiMessage *)calloc(n_msgs, sizeof(xAiMessage));
  out->blocks = (xAiContent *)calloc(n_blocks, sizeof(xAiContent));
  if (!out->msgs || !out->blocks) {
    view_free(out);
    return xErrno_NoMemory;
  }
  out->n_msgs   = n_msgs;
  out->n_blocks = n_blocks;

  size_t mi = 0; /* msg write index */
  size_t bi = 0; /* block write index */

  if (extra_system) {
    out->blocks[bi].type         = xAiContentType_Text;
    out->blocks[bi].u.text.text  = s->system_prompt;
    out->blocks[bi].u.text.len   = strlen(s->system_prompt);
    out->msgs[mi].role           = xAiRole_System;
    out->msgs[mi].contents       = &out->blocks[bi];
    out->msgs[mi].n              = 1;
    mi++;
    bi++;
  }

  for (size_t i = 0; i < s->n_history; ) {
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
        b->type                      = xAiContentType_ToolResult;
        b->u.tool_result.id          = m->tool_result_id;
        b->u.tool_result.output      = m->tool_result_output;
        b->u.tool_result.output_len  = m->tool_result_output_len;
        b->u.tool_result.is_error    = m->tool_result_is_error;
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

/* ── Provider callbacks (forward decls) ───────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg);
static void on_provider_tool_call(const xAiContent *call, void *arg);
static void on_provider_thinking(const char *chunk, size_t len, void *arg);
static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             const xAiUsage *usage, void *arg);

/* ── Submit ────────────────────────────────────────────────────── */

static xErrno submit_round(struct xAiQuery_ *q) {
  struct xAiSession_ *s = q->session;
  struct xAiAgent_   *a = (struct xAiAgent_ *)s->agent;

  struct view_ v;
  xErrno rc = view_build(q, &v);
  if (rc != xErrno_Ok) return rc;

  xAiProviderSubmitConf pc = {0};
  pc.model                 = s->model;
  pc.messages              = v.msgs;
  pc.n_messages            = v.n_msgs;
  pc.tools                 = (const xAiTool **)a->tools;
  pc.n_tools               = a->n_tools;
  pc.temperature           = -1;
  pc.max_tokens            = s->max_tokens;
  pc.stop                  = NULL;

  xAiProviderStreamCallbacks cbs = {0};
  cbs.on_text                    = on_provider_text;
  cbs.on_tool_call               = on_provider_tool_call;
  cbs.on_thinking                = on_provider_thinking;
  cbs.on_done                    = on_provider_done;

  assist_reset(q);
  reasoning_reset(q);
  pending_reset(q);
  q->turn++;

  rc = ai_provider_submit(a->provider, &pc, &cbs, q);
  view_free(&v);
  return rc;
}

/* ── Tool dispatch ─────────────────────────────────────────────── */

/* Look up a tool by name on the agent. Returns NULL if not found.
 *
 * Note: `a->tools` is `const xAiTool **` (array-of-handle-pointers,
 * matching provider.h's SubmitConf.tools contract). Each `a->tools[i]`
 * is itself a `const xAiTool *`, so the real handle is `*a->tools[i]`.
 * Do NOT short-circuit with a C-style cast: we burned that before
 * (04-23 provider_openai.c bug), the compiler can't catch it and the
 * lookup reads a bogus address. */
static xAiTool find_tool(struct xAiAgent_ *a, const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < a->n_tools; i++) {
    if (!a->tools[i]) continue;
    xAiTool t = *a->tools[i];
    const char *n = ai_tool_name(t);
    if (n && strcmp(n, name) == 0) return t;
  }
  return NULL;
}

/* Dispatch every buffered tool call, appending tool_result entries to
 * history. If the query is cancelled mid-dispatch we stop early and
 * let ai_session_finish_run surface Aborted.
 *
 * Returns xErrno_Ok if dispatch completed (successfully or with
 * individual tool errors folded back into history). Fatal failure
 * (OOM building result entries) aborts with the returned code. */
static xErrno dispatch_pending_tools(struct xAiQuery_ *q) {
  struct xAiSession_ *s = q->session;
  struct xAiAgent_   *a = (struct xAiAgent_ *)s->agent;

  for (size_t i = 0; i < q->n_pending && !q->cancelled; i++) {
    struct xAiQueryPending_ *p = &q->pending[i];

    if (s->cbs.on_tool) {
      s->cbs.on_tool((xAiSession)s, p->name, /*started=*/1, s->cbs.user_data);
    }

    xAiTool t = find_tool(a, p->name);
    xAiContent out = {0};
    int        is_error = 0;
    const char *out_text;
    size_t      out_text_len;
    char        err_buf[256];

    if (!t) {
      /* Unknown tool: feed the error back to the model rather than
       * aborting the whole run. The model is in a better position
       * to decide whether to retry with a different name or give
       * up. */
      snprintf(err_buf, sizeof(err_buf),
               "tool \"%s\" is not registered on this agent", p->name);
      out_text     = err_buf;
      out_text_len = strlen(err_buf);
      is_error     = 1;
    } else {
      xAiContent in = {0};
      in.type                 = xAiContentType_ToolUse;
      in.u.tool_use.id        = p->id;
      in.u.tool_use.name      = p->name;
      in.u.tool_use.args_json = p->args_json;
      xErrno trc = ai_tool_invoke(t, &in, &out);
      if (trc != xErrno_Ok) {
        snprintf(err_buf, sizeof(err_buf),
                 "tool handler returned error (xErrno=%d)", (int)trc);
        out_text     = err_buf;
        out_text_len = strlen(err_buf);
        is_error     = 1;
      } else if (out.type == xAiContentType_ToolResult) {
        out_text     = out.u.tool_result.output ? out.u.tool_result.output
                                                : "";
        out_text_len = out.u.tool_result.output_len
                           ? out.u.tool_result.output_len
                           : strlen(out_text);
        is_error     = out.u.tool_result.is_error ? 1 : 0;
      } else {
        /* Handler forgot to populate out — treat as error. */
        out_text     = "tool handler did not produce a tool_result";
        out_text_len = strlen(out_text);
        is_error     = 1;
      }
    }

    xErrno rc = ai_history_append_tool_result(s, p->id, out_text,
                                              out_text_len, is_error);

    if (s->cbs.on_tool) {
      s->cbs.on_tool((xAiSession)s, p->name, /*started=*/0, s->cbs.user_data);
    }

    if (rc != xErrno_Ok) return rc;
  }

  return xErrno_Ok;
}

/* ── Assistant-turn commit ─────────────────────────────────────── */

static void commit_assistant_turn(struct xAiQuery_ *q) {
  struct xAiSession_ *s = q->session;
  /* Order matters on the wire: the thinking block (if any) goes
   * FIRST inside the assistant turn, then the text, then each tool_use
   * entry. moonshot's kimi-k2.6 doesn't appear to care about the
   * exact ordering inside the message, but Anthropic's thinking
   * blocks are documented as coming first, and putting reasoning
   * before tool_calls matches every upstream example I've seen. */
  if (q->reasoning_len > 0) {
    (void)ai_history_append_thinking(s, q->reasoning_buf, q->reasoning_len);
  }
  if (q->assist_len > 0) {
    (void)ai_history_append_text(s, xAiRole_Assistant, q->assist_buf,
                                 q->assist_len);
  }
  for (size_t i = 0; i < q->n_pending; i++) {
    struct xAiQueryPending_ *p = &q->pending[i];
    (void)ai_history_append_tool_use(s, p->id, p->name, p->args_json);
  }
}

/* ── on_provider_done: three-way split ─────────────────────────
 *
 * The upstream provider calls us back with a terminal reason for the
 * round. Depending on the reason we take one of three branches:
 *
 *   - Error     : upstream transport / model error. Surface via on_error.
 *   - ToolUse   : provider wants more tool calls. Run handlers and,
 *                 if everything went well, submit another round.
 *   - Terminal  : the run is truly done. Translate the provider reason
 *                 to a caller-visible xAiDoneReason and finish.
 *
 * Each branch is factored into its own helper so the dispatcher below
 * stays a 3-way switch with zero embedded policy. Every helper returns
 * void and is responsible for calling ai_session_finish_run itself
 * once the run is over; the helper that submits the next round *does
 * not* call ai_session_finish_run because another round of callbacks
 * is coming.
 */

/* Map a provider stop reason to the caller-visible done reason for
 * runs that are *not* continuing into another tool-loop iteration. */
static xAiDoneReason translate_terminal(xAiProviderStopReason r,
                                        int user_cancel) {
  if (user_cancel) return xAiDoneReason_Aborted;
  switch (r) {
    case xAiProviderStop_EndTurn:    return xAiDoneReason_Completed;
    case xAiProviderStop_MaxTokens:  return xAiDoneReason_Completed;
    case xAiProviderStop_StopSeq:    return xAiDoneReason_Stopped;
    case xAiProviderStop_PromptLong: return xAiDoneReason_PromptTooLong;
    case xAiProviderStop_Error:      return xAiDoneReason_ModelError;
    case xAiProviderStop_Cancelled:  return xAiDoneReason_Aborted;
    case xAiProviderStop_ToolUse:
      /* Only reached if no pending calls survived (e.g. the model
       * advertised ToolUse but sent zero tool_calls). Treat as a
       * model error rather than silently looping. */
      return xAiDoneReason_ToolError;
  }
  return xAiDoneReason_ModelError;
}

/* Surface transport / model errors to the caller's on_error hook. The
 * run does not end here — the round's assistant output (if any) is
 * still committed and then translated to a terminal done reason. */
static void handle_error(struct xAiQuery_ *q, xErrno err) {
  struct xAiSession_ *s = q->session;
  if (err != xErrno_Ok && s->cbs.on_error) {
    s->cbs.on_error((xAiSession)s, err, NULL, s->cbs.user_data);
  }
}

/* Handle the ToolUse branch: honour max_turns, run every buffered
 * handler, and submit the next round. On any failure we finish the
 * run with the appropriate done reason. On success we return and
 * wait for the next on_provider_done callback. */
static void handle_tool_loop_continuation(struct xAiQuery_ *q) {
  struct xAiSession_ *s = q->session;
  int turn_limit = s->max_turns > 0 ? s->max_turns
                                    : XAI_SESSION_DEFAULT_MAX_TURNS;
  if (q->turn >= turn_limit) {
    /* Already emitted enough rounds; tell the caller we bailed. */
    ai_session_finish_run(s, xAiDoneReason_MaxTurns);
    return;
  }

  /* Run every pending handler; each appends a tool_result entry. */
  xErrno drc = dispatch_pending_tools(q);
  pending_reset(q);

  if (q->cancelled) {
    ai_session_finish_run(s, xAiDoneReason_Aborted);
    return;
  }
  if (drc != xErrno_Ok) {
    /* Catastrophic (e.g. OOM appending tool_result). Surface via
     * on_error to give the caller diagnostic detail, then close
     * the run. */
    if (s->cbs.on_error) {
      s->cbs.on_error((xAiSession)s, drc,
                      "failed to record tool_result in history",
                      s->cbs.user_data);
    }
    ai_session_finish_run(s, xAiDoneReason_ToolError);
    return;
  }

  /* Submit the next round. */
  xErrno src = submit_round(q);
  if (src != xErrno_Ok) {
    if (s->cbs.on_error) {
      s->cbs.on_error((xAiSession)s, src,
                      "failed to submit follow-up tool round",
                      s->cbs.user_data);
    }
    ai_session_finish_run(s, xAiDoneReason_ModelError);
  }
}

/* Handle the terminal branch: translate the provider's stop reason
 * to a caller-visible done reason and close the run. */
static void handle_terminal(struct xAiQuery_ *q, xAiProviderStopReason reason,
                            int user_cancel) {
  ai_session_finish_run(q->session, translate_terminal(reason, user_cancel));
}

/* ── Provider callbacks ────────────────────────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg) {
  struct xAiQuery_ *q = (struct xAiQuery_ *)arg;
  if (q->cancelled) return;

  (void)assist_append(q, chunk, len);

  struct xAiSession_ *s = q->session;
  if (s->cbs.on_text) {
    s->cbs.on_text((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

static void on_provider_tool_call(const xAiContent *call, void *arg) {
  struct xAiQuery_ *q = (struct xAiQuery_ *)arg;
  if (q->cancelled || !call || call->type != xAiContentType_ToolUse) return;
  /* Buffer; actual dispatch happens from on_provider_done when we
   * know the assistant message is complete. */
  (void)pending_append(q, call);
}

/* Absorb a reasoning / thinking delta. We buffer it into the
 * per-round reasoning_buf (the final history entry is committed in
 * commit_assistant_turn so it lands alongside — and before — the
 * round's text and tool_use blocks inside the same assistant turn),
 * AND forward it live to the caller if they asked for a thinking
 * channel. Non-thinking callers just leave cbs.on_thinking NULL and
 * the delta is silently buffered for the next-round echo-back, which
 * servers like kimi-k2.6 require. */
static void on_provider_thinking(const char *chunk, size_t len, void *arg) {
  struct xAiQuery_ *q = (struct xAiQuery_ *)arg;
  if (q->cancelled) return;

  (void)reasoning_append(q, chunk, len);

  struct xAiSession_ *s = q->session;
  if (s->cbs.on_thinking) {
    s->cbs.on_thinking((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             const xAiUsage *usage, void *arg) {
  struct xAiQuery_ *q = (struct xAiQuery_ *)arg;

  /* Fold this round's usage into the running total BEFORE any
   * branching — we want the accounting to be correct whether the
   * run ends here or continues into another tool-loop round. If the
   * provider didn't report usage this round, the accumulator stays
   * where it was. */
  usage_accumulate(q, usage);

  int user_cancel = (reason == xAiProviderStop_Cancelled) || q->cancelled;

  /* Surface transport / model errors before anything else so the
   * caller's on_error fires in order with the round. */
  if (reason == xAiProviderStop_Error) {
    handle_error(q, err);
  }

  /* Commit the assistant turn into history regardless of outcome —
   * any text or tool_use the model managed to emit is legitimate
   * output. */
  commit_assistant_turn(q);

  /* Continue the tool loop iff: (a) not cancelled, (b) provider said
   * ToolUse AND we buffered >=1 tool call, (c) max_turns not exceeded. */
  int can_continue = !user_cancel && reason == xAiProviderStop_ToolUse &&
                     q->n_pending > 0;

  if (can_continue) {
    handle_tool_loop_continuation(q);
    return;
  }

  handle_terminal(q, reason, user_cancel);
}

/* ── Internal API (declared in query_private.h) ────────────────── */

void ai_query_arm(struct xAiQuery_ *q, struct xAiSession_ *session) {
  q->session   = session;
  q->running   = 1;
  q->cancelled = 0;
  q->turn      = 0;
  usage_reset(q);
}

xErrno ai_query_submit(struct xAiQuery_ *q) {
  return submit_round(q);
}

void ai_query_cancel_mark(struct xAiQuery_ *q) {
  q->cancelled = 1;
}

void ai_query_dispose(struct xAiQuery_ *q) {
  free(q->assist_buf);
  q->assist_buf = NULL;
  q->assist_len = 0;
  q->assist_cap = 0;
  free(q->reasoning_buf);
  q->reasoning_buf = NULL;
  q->reasoning_len = 0;
  q->reasoning_cap = 0;
  pending_reset(q);
  free(q->pending);
  q->pending     = NULL;
  q->cap_pending = 0;
}

/* ── Called from ai_session_finish_run ─────────────────────────
 *
 * session.c's ai_session_finish_run snapshots usage, invokes the
 * caller's on_done, then calls this hook so query.c can clear the
 * round-scoped state (buffers + pending + flags) and hand the Query
 * back to its idle shape. Kept as its own function (rather than
 * inlined in ai_query_dispose) because finish_run runs after every
 * round and must not free the buffers — only dispose does. */
void ai_query_reset_round(struct xAiQuery_ *q) {
  assist_reset(q);
  reasoning_reset(q);
  pending_reset(q);
  usage_reset(q);
  q->running   = 0;
  q->cancelled = 0;
  q->turn      = 0;
}
