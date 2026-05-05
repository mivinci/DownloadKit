/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query.c - xAgentQuery implementation
 *
 * One Query is one end-to-end run of the provider / tool loop:
 *
 *     Create → Run(msgs[]) → round 1 → (tools?) → round 2 → ... → on_done
 *
 * Session owns the durable state (conversation history,
 * configuration, caller callbacks). Each accepted xAgentSessionInput()
 * heap-allocates a fresh Query via xAgentQueryCreate, hands it the
 * full message list it should run on via xAgentQueryRun, and destroys
 * it from its forwarding on_done once the terminal callback has
 * fired. Query owns the transient state of that single run (input
 * snapshot, produced turns, streaming buffers, pending tool calls,
 * cumulative usage, running/cancelled flags) and fires its own
 * @c q->cbs.* stream — the Session installs forwarding shims that
 * re-dispatch those calls to the caller's xAgentSessionCallbacks.
 *
 * Phase β: the Query is stateless w.r.t. the Session's history.
 * view_build walks @c q->inputs + @c q->produced (not
 * @c s->history). New assistant turns and tool_result entries land
 * in @c q->produced; on terminal on_done the Session pulls
 * @c produced out via ai_query_take_produced() and merges it into
 * @c s->history before destroying the Query.
 *
 * This file is fully self-contained: the Query carries its own
 * provider, tools, model, and limits — it never reaches back into
 * any Session for runtime configuration. The optional session
 * back-pointer is used exclusively by the observational
 * xAgentQuerySession() API and the single-flight guard.
 */

#include "query_private.h"
#include "session_private.h"

#include "agent_private.h"
#include "provider_private.h" /* ai_provider_submit / ai_provider_cancel   */
#include "tool_private.h"     /* ai_tool_name / ai_tool_invoke             */

#include <xagent/message.h>
#include <xagent/provider.h>
#include <xagent/query.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/array.h>
#include <xbuf/buf.h>

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

/* ── Per-thread free list for Query objects ───────────────────────
 *
 * xAgentQueryCreate / Destroy are called on every Session input cycle,
 * making them a hot path.  A per-thread free list avoids
 * malloc/free overhead entirely — zero locks, zero atomics.
 *
 * We reuse query->next as the freelist link pointer (zero extra
 * memory). A per-thread cap prevents unbounded caching.
 */
#define QUERY_FREELIST_CAP 16

struct query_freelist {
  struct xAgentQuery_ *head;
  size_t            count;
};

static __thread struct query_freelist tl_qfree = {NULL, 0};

static struct xAgentQuery_ *query_alloc(void) {
  if (tl_qfree.head) {
    struct xAgentQuery_ *q = tl_qfree.head;
    tl_qfree.head       = q->next;
    tl_qfree.count--;
    q->next = NULL;
    return q;
  }
  return (struct xAgentQuery_ *)calloc(1, sizeof(struct xAgentQuery_));
}

static void query_free(struct xAgentQuery_ *q) {
  if (tl_qfree.count >= QUERY_FREELIST_CAP) {
    free(q);
    return;
  }
  q->next          = tl_qfree.head;
  tl_qfree.head    = q;
  tl_qfree.count++;
}

/* ── xArray callbacks for turn-entry arrays ──────────────────────
 *
 * Both inputs_arr and produced_arr store struct xAgentSessionMsg_
 * elements whose heap-owned strings must be freed on removal.
 * The release callback wraps ai_session_msg_free(); no retain
 * callback is needed because callers fill zero-initialised slots
 * manually after xArrayPush(). */

static void query_msg_release(void *elem) {
  ai_session_msg_free((struct xAgentSessionMsg_ *)elem);
}

static const xArrayCallbacks kMsgCbs = { NULL, query_msg_release, NULL };

/* Push a new zero-initialised slot onto a turn-entry array. The
 * returned slot has @c created_at_ms pre-filled with the current
 * wall-clock time so downstream splice into session history
 * preserves the production instant (streamed assistant chunk,
 * completed tool call, etc.) rather than the later time at which
 * the on_done handler copies the entries over. */
static struct xAgentSessionMsg_ *msg_push(xArray *arrp) {
  struct xAgentSessionMsg_ *slot =
    (struct xAgentSessionMsg_ *)xArrayPush(arrp);
  if (slot) slot->created_at_ms = ai_now_unix_ms_();
  return slot;
}

/* Append a single-content text entry with role @p role. The text is
 * duplicated. Used to materialise user text and assistant text into
 * turn-entry storage. */
static xErrno turn_buf_append_text(xArray *arrp, xAgentRole role,
                                   const char *text, size_t len) {
  struct xAgentSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
  slot->kind = xAgentSessionEntry_Text;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      xArrayPop(*arrp);
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

/* Append an assistant thinking entry. @p text is duplicated. */
static xErrno turn_buf_append_thinking(xArray *arrp, const char *text,
                                       size_t len) {
  struct xAgentSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAgentRole_Assistant;
  slot->kind = xAgentSessionEntry_Thinking;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      xArrayPop(*arrp);
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

/* Append an assistant tool_use entry. Every string is duplicated. */
static xErrno turn_buf_append_tool_use(xArray *arrp, const char *id,
                                       const char *name, const char *args) {
  struct xAgentSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role          = xAgentRole_Assistant;
  slot->kind          = xAgentSessionEntry_ToolUse;
  slot->tool_use_id   = dup_cstr(id ? id : "");
  slot->tool_use_name = dup_cstr(name ? name : "");
  slot->tool_use_args = dup_cstr(args ? args : "{}");
  if (!slot->tool_use_id || !slot->tool_use_name || !slot->tool_use_args) {
    /* xArrayPop calls query_msg_release which frees the partial
     * fields we just allocated. */
    xArrayPop(*arrp);
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Append a tool_result entry. The output is duplicated. */
static xErrno turn_buf_append_tool_result(xArray *arrp, const char *id,
                                          const char *output,
                                          size_t output_len, int is_error) {
  struct xAgentSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role           = xAgentRole_Tool;
  slot->kind           = xAgentSessionEntry_ToolResult;
  slot->tool_result_id = dup_cstr(id ? id : "");
  if (output_len > 0) {
    slot->tool_result_output     = dup_bytes(output, output_len);
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
    /* xArrayPop calls query_msg_release which frees partial fields. */
    xArrayPop(*arrp);
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Deep-copy one incoming xAgentMessage into @p buf as one or more
 * turn-entry records. We pick the shape by role+content:
 *
 *   - System / User / Tool with one-or-more Text blocks: concatenate
 *     every Text block into a single Text entry (the wire protocol
 *     re-splits if it cares, and this keeps our history compact).
 *   - Assistant with any mix of Text / Thinking / ToolUse blocks:
 *     one entry per block, preserving block order (view_build folds
 *     consecutive Assistant entries back into one message).
 *   - Tool with a ToolResult block: one ToolResult entry.
 *
 * Unknown / empty block combinations are skipped silently. */
static xErrno turn_buf_append_message(xArray *arrp, const xAgentMessage *msg) {
  if (!msg) return xErrno_InvalidArg;

  if (msg->role == xAgentRole_Assistant) {
    for (size_t i = 0; i < msg->n; i++) {
      const xAgentContent *c = &msg->contents[i];
      xErrno            rc;
      switch (c->type) {
      case xAgentContentType_Text:
        rc = turn_buf_append_text(arrp, xAgentRole_Assistant,
                                  c->u.text.text, c->u.text.len);
        break;
      case xAgentContentType_Thinking:
        rc = turn_buf_append_thinking(arrp, c->u.thinking.text,
                                      c->u.thinking.len);
        break;
      case xAgentContentType_ToolUse:
        rc = turn_buf_append_tool_use(arrp, c->u.tool_use.id,
                                      c->u.tool_use.name,
                                      c->u.tool_use.args_json);
        break;
      default:
        rc = xErrno_Ok; /* skip */
      }
      if (rc != xErrno_Ok) return rc;
    }
    return xErrno_Ok;
  }

  if (msg->role == xAgentRole_Tool) {
    for (size_t i = 0; i < msg->n; i++) {
      const xAgentContent *c = &msg->contents[i];
      if (c->type != xAgentContentType_ToolResult) continue;
      xErrno rc = turn_buf_append_tool_result(
        arrp, c->u.tool_result.id, c->u.tool_result.output,
        c->u.tool_result.output_len, c->u.tool_result.is_error);
      if (rc != xErrno_Ok) return rc;
    }
    return xErrno_Ok;
  }

  /* System / User: concatenate every Text block into one entry. */
  size_t total = 0;
  for (size_t i = 0; i < msg->n; i++) {
    if (msg->contents[i].type == xAgentContentType_Text) {
      total += msg->contents[i].u.text.len;
    }
  }
  if (total == 0) {
    return turn_buf_append_text(arrp, msg->role, NULL, 0);
  }
  char *concat = (char *)malloc(total + 1);
  if (!concat) return xErrno_NoMemory;
  size_t off = 0;
  for (size_t i = 0; i < msg->n; i++) {
    if (msg->contents[i].type == xAgentContentType_Text) {
      size_t k = msg->contents[i].u.text.len;
      if (k) memcpy(concat + off, msg->contents[i].u.text.text, k);
      off += k;
    }
  }
  concat[total]               = '\0';
  struct xAgentSessionMsg_ *slot = msg_push(arrp);
  if (!slot) {
    free(concat);
    return xErrno_NoMemory;
  }
  slot->role     = msg->role;
  slot->kind     = xAgentSessionEntry_Text;
  slot->text     = concat;
  slot->text_len = total;
  return xErrno_Ok;
}

/* ── Pending tool-call bookkeeping ──────────────────────────────── */

static void pending_release(void *elem) {
  struct xAgentQueryPending_ *p = (struct xAgentQueryPending_ *)elem;
  if (!p) return;
  free(p->id);
  free(p->name);
  free(p->args_json);
  memset(p, 0, sizeof(*p));
}

static const xArrayCallbacks kPendingCbs = { NULL, pending_release, NULL };

/* ── Tool-confirmation resolver ──────────────────────────────────────
 *
 * When a needs_confirm tool arrives in dispatch_pending_tools we do
 * NOT invoke the handler. Instead we enqueue the call into
 * async_pending_arr with stage=AwaitingConfirm and hand the host a
 * resolver handle via q->cbs.on_tool_confirm. The host eventually
 * calls xAgentToolConfirmResolve(resolver, Allow|Reject, reason)
 * which routes back here to either:
 *   - Allow  : transition the entry to stage=Running and invoke the
 *              tool handler synchronously (the handler itself may
 *              be sync or async — if it returns xErrno_Pending the
 *              entry just stays in async_pending_arr as Running).
 *   - Reject : fabricate an is_error=1 tool_result and feed it back
 *              through ai_query_async_tool_complete (which is the
 *              same path async tools take when they finish).
 *
 * Because the host might resolve after the query has been
 * cancelled / destroyed, every resolver carries a generation
 * counter that is invalidated on destroy; stale resolvers simply
 * free themselves and return.
 */

struct xAgentToolConfirmResolver_ {
  struct xAgentQuery_ *q;     /* owning query; NULL = invalidated */
  char             *id;    /* tool_use_id, owned copy          */
  int               done;  /* 1 after resolve() ran once       */
};

static struct xAgentToolConfirmResolver_ *
confirm_resolver_new_(struct xAgentQuery_ *q, const char *tool_use_id) {
  struct xAgentToolConfirmResolver_ *r =
    (struct xAgentToolConfirmResolver_ *)calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->q  = q;
  r->id = dup_cstr(tool_use_id ? tool_use_id : "");
  if (!r->id) {
    free(r);
    return NULL;
  }
  return r;
}

static void confirm_resolver_free_(struct xAgentToolConfirmResolver_ *r) {
  if (!r) return;
  free(r->id);
  free(r);
}

/* Invalidate a resolver so that any still-pending xAgentToolConfirmResolve()
 * from the host becomes a no-op. Called when the entry is removed
 * from async_pending_arr (Reject path, cancel, destroy). */
static void confirm_resolver_invalidate_(struct xAgentToolConfirmResolver_ *r) {
  if (!r) return;
  r->q = NULL;
}

/* ── Async pending tool-call bookkeeping ────────────────────────────── */

static void async_pending_release(void *elem) {
  struct xAgentQueryAsyncTool_ *a = (struct xAgentQueryAsyncTool_ *)elem;
  if (!a) return;
  free(a->id);
  free(a->name);
  free(a->args_json);
  if (a->resolver) {
    /* Invalidate but do NOT free: the resolver handle lives in the
     * host\u2019s hands. A late xAgentToolConfirmResolve() must be a
     * safe no-op, so the host\u2019s pointer has to remain valid
     * memory. The resolver is freed from inside
     * xAgentToolConfirmResolve() (where we know it\u2019s been dequeued
     * exactly once). If the host never resolves, the handle leaks
     * \u2014 a small, per-cancelled-call cost we accept in exchange for
     * crash-safety. */
    confirm_resolver_invalidate_(
        (struct xAgentToolConfirmResolver_ *)a->resolver);
  }
  memset(a, 0, sizeof(*a));
}

static const xArrayCallbacks kAsyncPendingCbs = { NULL, async_pending_release, NULL };

static void pending_reset(struct xAgentQuery_ *q) {
  xArrayReset(q->pending_arr);
}

/* Append one pending tool_call. Copies every string. */
static xErrno pending_append(struct xAgentQuery_ *q, const xAgentContent *call) {
  struct xAgentQueryPending_ *slot =
    (struct xAgentQueryPending_ *)xArrayPush(&q->pending_arr);
  if (!slot) return xErrno_NoMemory;
  slot->id   = dup_cstr(call->u.tool_use.id ? call->u.tool_use.id : "");
  slot->name = dup_cstr(call->u.tool_use.name ? call->u.tool_use.name : "");
  slot->args_json =
    dup_cstr(call->u.tool_use.args_json ? call->u.tool_use.args_json : "{}");
  if (!slot->id || !slot->name || !slot->args_json) {
    /* xArrayPop calls pending_release which frees partial fields. */
    xArrayPop(q->pending_arr);
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* ── Round-scoped streaming accumulators ───────────────────────── */

/* Append one byte range to the current-round assistant text buffer.
 * The xBuffer is lazy-created on first append so queries that never
 * receive text (e.g. tool-use only) avoid a needless allocation. */
static xErrno assist_append(struct xAgentQuery_ *q, const char *chunk,
                            size_t len) {
  if (len == 0) return xErrno_Ok;
  if (!q->assist) {
    q->assist = xBufferCreate(256);
    if (!q->assist) return xErrno_NoMemory;
  }
  return xBufferAppend(&q->assist, chunk, len);
}

static void assist_reset(struct xAgentQuery_ *q) {
  if (q->assist) xBufferReset(q->assist);
}

/* Same shape as assist_append, but for the current-round reasoning /
 * thinking stream. Kept separate so the two never race to the same
 * buffer and so view_build can emit them as distinct content blocks
 * on the wire. */
static xErrno reasoning_append(struct xAgentQuery_ *q, const char *chunk,
                               size_t len) {
  if (len == 0) return xErrno_Ok;
  if (!q->reasoning) {
    q->reasoning = xBufferCreate(256);
    if (!q->reasoning) return xErrno_NoMemory;
  }
  return xBufferAppend(&q->reasoning, chunk, len);
}

static void reasoning_reset(struct xAgentQuery_ *q) {
  if (q->reasoning) xBufferReset(q->reasoning);
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
static void usage_accumulate(struct xAgentQuery_ *q, const xAgentUsage *round) {
  if (!round) return;
  q->saw_usage = 1;

  /* prompt_tokens is a snapshot: each round's value represents the
   * TOTAL input the provider saw on that round (system prompt +
   * tools + full history + current turn). It is NOT incremental —
   * later rounds always report a value >= earlier ones because the
   * conversation only grows. We therefore take the maximum (i.e.
   * the last round's value) rather than adding, which would
   * inflate the count by a factor-of-rounds after tool loops.
   *
   * completion_tokens and total_tokens ARE incremental per round
   * (each round generates its own output), so those remain additive.
   *
   * We also capture the FIRST round's prompt_tokens separately so
   * the calibrator can compare it against the gate's pre-submit
   * estimate (which was computed before any tool-loop rounds). */
#define XAGENT_FOLD_MAX(field)                                                 \
  do {                                                                       \
    if (round->field >= 0) {                                                 \
      q->usage.field = (q->usage.field < 0) ? round->field                  \
                         : (q->usage.field > round->field                    \
                              ? q->usage.field                               \
                              : round->field);                               \
    }                                                                        \
  } while (0)

#define XAGENT_FOLD_ADD(field)                                                 \
  do {                                                                       \
    if (round->field >= 0) {                                                 \
      q->usage.field =                                                       \
        (q->usage.field < 0) ? round->field : q->usage.field + round->field; \
    }                                                                        \
  } while (0)

  XAGENT_FOLD_MAX(prompt_tokens);
  XAGENT_FOLD_ADD(completion_tokens);
  XAGENT_FOLD_ADD(total_tokens);

#undef XAGENT_FOLD_MAX
#undef XAGENT_FOLD_ADD

  /* Capture first-round prompt_tokens for calibrator use. The gate
   * estimate (last_prompt_estimate) is computed before the Query is
   * submitted, so it maps to the FIRST round's prompt_tokens, not
   * the accumulated maximum. Subsequent rounds add tool_results to
   * the prompt — those extra tokens were never estimated by the gate
   * and would systematically inflate the ratio if included. */
  if (q->turn == 1 && round->prompt_tokens >= 0 &&
      q->first_round_prompt_tokens < 0) {
    q->first_round_prompt_tokens = round->prompt_tokens;
  }
}

static void usage_reset(struct xAgentQuery_ *q) {
  q->saw_usage                    = 0;
  q->usage.prompt_tokens          = -1;
  q->usage.completion_tokens      = -1;
  q->usage.total_tokens           = -1;
  q->first_round_prompt_tokens    = -1;
}

/* ── Submit-view construction ──────────────────────────────────── */

/* The transient arrays passed to provider_submit. The provider
 * borrows them only for the duration of submit(); we tear them down
 * immediately after. */
struct view_ {
  xAgentMessage *msgs;   /* n_msgs entries                             */
  xAgentContent *blocks; /* n_blocks entries, referenced by msgs[i]    */
  size_t      n_msgs;
  size_t      n_blocks;
};

static void view_free(struct view_ *v) {
  free(v->msgs);
  free(v->blocks);
  memset(v, 0, sizeof(*v));
}

/* Fold consecutive Assistant entries into one xAgentMessage (possibly
 * carrying thinking / text / tool_use blocks together); every other
 * entry maps 1:1 to an xAgentMessage.
 *
 * The entry sequence the builder walks is @c q->inputs concatenated
 * with @c q->produced: inputs carries whatever the Session handed
 * to xAgentQueryRun (System prompt + rolling history + the new user
 * turn), produced carries everything this run has emitted so far
 * (assistant turns + tool_results from previous rounds). The Query
 * never reads @c s->history; session.c is responsible for baking
 * the system prompt into @c inputs before Run. */
static xErrno view_build(struct xAgentQuery_ *q, struct view_ *out) {
  memset(out, 0, sizeof(*out));

  /* We present inputs and produced as one logical sequence; the
   * two arrays make the fold loop below simpler than a runtime
   * concat. */
  struct xAgentSessionMsg_ *arrs[2] = {
    (struct xAgentSessionMsg_ *)xArrayData(q->inputs_arr),
    (struct xAgentSessionMsg_ *)xArrayData(q->produced_arr)
  };
  size_t lens[2] = { xArrayLen(q->inputs_arr), xArrayLen(q->produced_arr) };
  size_t n_total = lens[0] + lens[1];
  if (n_total == 0) return xErrno_InvalidArg;

  /* Helper: address of logical entry i (across arrs[0]++arrs[1]). */
#define XAGENT_ENTRY(i) \
  (((i) < lens[0]) ? &arrs[0][(i)] : &arrs[1][(i) - lens[0]])

  /* Pass 1: count output messages and content blocks. */
  size_t n_msgs   = 0;
  size_t n_blocks = 0;
  for (size_t i = 0; i < n_total;) {
    struct xAgentSessionMsg_ *m = XAGENT_ENTRY(i);
    if (m->role == xAgentRole_Assistant) {
      size_t j = i;
      while (j < n_total && XAGENT_ENTRY(j)->role == xAgentRole_Assistant) j++;
      n_msgs += 1;
      n_blocks += (j - i);
      i = j;
    } else {
      n_msgs += 1;
      n_blocks += 1;
      i += 1;
    }
  }

  out->msgs   = (xAgentMessage *)calloc(n_msgs, sizeof(xAgentMessage));
  out->blocks = (xAgentContent *)calloc(n_blocks, sizeof(xAgentContent));
  if (!out->msgs || !out->blocks) {
    view_free(out);
    return xErrno_NoMemory;
  }
  out->n_msgs   = n_msgs;
  out->n_blocks = n_blocks;

  /* Pass 2: populate. */
  size_t mi = 0;
  size_t bi = 0;
  for (size_t i = 0; i < n_total;) {
    struct xAgentSessionMsg_ *m = XAGENT_ENTRY(i);
    if (m->role == xAgentRole_Assistant) {
      size_t block_start = bi;
      size_t j           = i;
      while (j < n_total && XAGENT_ENTRY(j)->role == xAgentRole_Assistant) {
        struct xAgentSessionMsg_ *mm = XAGENT_ENTRY(j);
        xAgentContent            *b  = &out->blocks[bi++];
        if (mm->kind == xAgentSessionEntry_Text) {
          b->type        = xAgentContentType_Text;
          b->u.text.text = mm->text ? mm->text : "";
          b->u.text.len  = mm->text_len;
        } else if (mm->kind == xAgentSessionEntry_Thinking) {
          b->type            = xAgentContentType_Thinking;
          b->u.thinking.text = mm->text ? mm->text : "";
          b->u.thinking.len  = mm->text_len;
        } else if (mm->kind == xAgentSessionEntry_ToolUse) {
          b->type                 = xAgentContentType_ToolUse;
          b->u.tool_use.id        = mm->tool_use_id;
          b->u.tool_use.name      = mm->tool_use_name;
          b->u.tool_use.args_json = mm->tool_use_args;
        }
        j++;
      }
      out->msgs[mi].role     = xAgentRole_Assistant;
      out->msgs[mi].contents = &out->blocks[block_start];
      out->msgs[mi].n        = bi - block_start;
      mi++;
      i = j;
    } else {
      xAgentContent *b = &out->blocks[bi];
      if (m->kind == xAgentSessionEntry_ToolResult) {
        b->type                     = xAgentContentType_ToolResult;
        b->u.tool_result.id         = m->tool_result_id;
        b->u.tool_result.output     = m->tool_result_output;
        b->u.tool_result.output_len = m->tool_result_output_len;
        b->u.tool_result.is_error   = m->tool_result_is_error;
      } else {
        b->type        = xAgentContentType_Text;
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

#undef XAGENT_ENTRY
  return xErrno_Ok;
}

/* ── Provider callbacks (forward decls) ───────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg);
static void on_provider_tool_call(const xAgentContent *call, void *arg);
static void on_provider_thinking(const char *chunk, size_t len, void *arg);
static void on_provider_done(xAgentProviderStopReason reason, xErrno err,
                             const xAgentUsage *usage, const char *errmsg,
                             void *arg);

/* ── Submit ────────────────────────────────────────────────────── */

static xErrno submit_round(struct xAgentQuery_ *q) {
  struct view_ v;
  xErrno       rc = view_build(q, &v);
  if (rc != xErrno_Ok) return rc;

  xAgentProviderSubmitConf pc = {0};
  pc.model                 = q->model;
  pc.messages              = v.msgs;
  pc.n_messages            = v.n_msgs;
  pc.tools                 = q->tools;
  pc.tools_count           = q->tools_count;
  pc.temperature           = -1;
  pc.max_tokens            = q->max_tokens;
  pc.stop                  = NULL;

  xAgentProviderStreamCallbacks cbs = {0};
  cbs.on_text                    = on_provider_text;
  cbs.on_tool_call               = on_provider_tool_call;
  cbs.on_thinking                = on_provider_thinking;
  cbs.on_done                    = on_provider_done;

  assist_reset(q);
  reasoning_reset(q);
  pending_reset(q);
  q->turn++;

  rc = ai_provider_submit(q->provider, &pc, &cbs, q);
  view_free(&v);
  return rc;
}

/* ── Tool dispatch ─────────────────────────────────────────────── */

/* Look up a tool by name on the agent. Returns NULL if not found.
 *
 * Note: `a->tools` is `const xAgentTool **` (array-of-handle-pointers,
 * matching provider.h's SubmitConf.tools contract). Each `a->tools[i]`
 * is itself a `const xAgentTool *`, so the real handle is `*a->tools[i]`.
 * Do NOT short-circuit with a C-style cast: we burned that before
 * (04-23 provider_openai.c bug), the compiler can't catch it and the
 * lookup reads a bogus address. */
static xAgentTool find_tool(const xAgentTool **tools, size_t tools_count,
                        const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < tools_count; i++) {
    if (!tools[i]) continue;
    xAgentTool     t = *tools[i];
    const char *n = ai_tool_name(t);
    if (n && strcmp(n, name) == 0) return t;
  }
  return NULL;
}

/* Dispatch every buffered tool call, appending tool_result entries to
 * @c q->produced. If the query is cancelled mid-dispatch we stop
 * early and let the caller surface Aborted.
 *
 * Returns xErrno_Ok if dispatch completed (successfully or with
 * individual tool errors folded back into produced). Fatal failure
 * (OOM building result entries) aborts with the returned code.
 *
 * Forward declarations needed for async completion path. */
static void query_finalize(struct xAgentQuery_ *q, xAgentDoneReason reason);
static xErrno submit_round(struct xAgentQuery_ *q);

/* Invoke a tool handler synchronously on behalf of dispatch_pending_tools
 * (or the resolver Allow path). On xErrno_Pending the call has been
 * handed off to async_pending_arr; the caller must NOT append a
 * tool_result \u2014 the async completion path will do it.
 *
 * On any other return the function writes a tool_result entry into
 * produced_arr and fires on_tool(started=0). For synchronous success
 * it also fires on_tool(started=1) before the handler and (=0) after.
 * For the Unknown / handler-error paths on_tool is *not* fired
 * (the tool never really ran).
 *
 * Returns xErrno_Ok when the call has been fully accounted for, or
 * a fatal xErrno (currently only NoMemory when the async slot or
 * the tool_result slot fails to allocate).
 */
static xErrno invoke_tool_inline_(struct xAgentQuery_ *q, xAgentTool t,
                                  const char *tool_use_id,
                                  const char *tool_name,
                                  const char *args_json) {
  xAgentContent in = {0};
  in.type = xAgentContentType_ToolUse;
  in.u.tool_use.id        = tool_use_id;
  in.u.tool_use.name      = tool_name;
  in.u.tool_use.args_json = args_json;

  if (q->cbs.on_tool) {
    q->cbs.on_tool((xAgentQuery)q, tool_name, /*started=*/1, q->cbs.user_data);
  }

  xAgentContent out = {0};
  xErrno      trc = ai_tool_invoke(t, (xAgentQuery)q, &in, &out);

  if (trc == xErrno_Pending) {
    /* Asynchronous tool: the handler will call on_done_fn later.
     * Register the in-flight entry so we can track it. The
     * on_tool(started=0) callback is deferred until the async
     * completion fires (see ai_query_async_tool_complete). */
    struct xAgentQueryAsyncTool_ *slot =
      (struct xAgentQueryAsyncTool_ *)xArrayPush(&q->async_pending_arr);
    if (!slot) return xErrno_NoMemory;
    slot->tool      = t;
    slot->done_fn   = ai_tool_on_done_fn(t);
    slot->done_ud   = ai_tool_on_done_ud(t);
    slot->cancel_fn = ai_tool_on_cancel_fn(t);
    slot->cancel_ud = ai_tool_on_cancel_ud(t);
    slot->id        = dup_cstr(tool_use_id);
    slot->name      = dup_cstr(tool_name);
    slot->args_json = dup_cstr(args_json);
    slot->stage     = XAGENT_ASYNC_STAGE_RUNNING;
    slot->resolver  = NULL;
    if (!slot->id || !slot->name || !slot->args_json) {
      xArrayPop(q->async_pending_arr);
      return xErrno_NoMemory;
    }
    return xErrno_Ok;
  }

  const char *out_text;
  size_t      out_text_len;
  int         is_error = 0;
  char        err_buf[256];

  if (trc != xErrno_Ok) {
    snprintf(err_buf, sizeof(err_buf),
             "tool handler returned error (xErrno=%d)", (int)trc);
    out_text     = err_buf;
    out_text_len = strlen(err_buf);
    is_error     = 1;
  } else if (out.type == xAgentContentType_ToolResult) {
    out_text     = out.u.tool_result.output ? out.u.tool_result.output : "";
    out_text_len = out.u.tool_result.output_len
                     ? out.u.tool_result.output_len
                     : strlen(out_text);
    is_error     = out.u.tool_result.is_error ? 1 : 0;
  } else {
    out_text     = "tool handler did not produce a tool_result";
    out_text_len = strlen(out_text);
    is_error     = 1;
  }

  xErrno rc = turn_buf_append_tool_result(&q->produced_arr, tool_use_id,
                                          out_text, out_text_len, is_error);

  if (q->cbs.on_tool) {
    q->cbs.on_tool((xAgentQuery)q, tool_name, /*started=*/0, q->cbs.user_data);
  }

  return rc;
}

/* Append a synthetic is_error=1 tool_result directly to produced_arr.
 * Used by the Reject path of the confirmation gate and by the
 * unknown-tool path of dispatch_pending_tools. */
static xErrno append_tool_error_(struct xAgentQuery_ *q, const char *tool_use_id,
                                 const char *message) {
  const char *m = message ? message : "tool error";
  return turn_buf_append_tool_result(&q->produced_arr, tool_use_id, m,
                                     strlen(m), /*is_error=*/1);
}

/* Register a pending tool call that is blocked on user confirmation.
 * Pushes an async_pending entry with stage=AwaitingConfirm and allocates
 * a resolver handle; the handle is returned so the caller can hand it
 * to on_tool_confirm. On any failure the function rolls back all
 * side-effects and returns NULL. */
static struct xAgentToolConfirmResolver_ *
register_confirm_pending_(struct xAgentQuery_ *q, xAgentTool t,
                          const char *tool_use_id, const char *tool_name,
                          const char *args_json) {
  struct xAgentQueryAsyncTool_ *slot =
    (struct xAgentQueryAsyncTool_ *)xArrayPush(&q->async_pending_arr);
  if (!slot) return NULL;
  slot->tool      = t;
  slot->done_fn   = ai_tool_on_done_fn(t);
  slot->done_ud   = ai_tool_on_done_ud(t);
  slot->cancel_fn = ai_tool_on_cancel_fn(t);
  slot->cancel_ud = ai_tool_on_cancel_ud(t);
  slot->id        = dup_cstr(tool_use_id);
  slot->name      = dup_cstr(tool_name);
  slot->args_json = dup_cstr(args_json);
  slot->stage     = XAGENT_ASYNC_STAGE_AWAITING_CONFIRM;
  slot->resolver  = NULL;
  if (!slot->id || !slot->name || !slot->args_json) {
    xArrayPop(q->async_pending_arr);
    return NULL;
  }

  struct xAgentToolConfirmResolver_ *r =
    confirm_resolver_new_(q, tool_use_id);
  if (!r) {
    xArrayPop(q->async_pending_arr);
    return NULL;
  }
  slot->resolver = r;
  return r;
}

static xErrno dispatch_pending_tools(struct xAgentQuery_ *q) {
  for (size_t i = 0; i < xArrayLen(q->pending_arr) && !q->cancelled; i++) {
    struct xAgentQueryPending_ *p =
      (struct xAgentQueryPending_ *)xArrayAt(q->pending_arr, i);

    xAgentTool t = find_tool(q->tools, q->tools_count, p->name);

    if (!t) {
      /* Unknown tool: feed the error back to the model rather than
       * aborting the whole run. The model is in a better position
       * to decide whether to retry with a different name or give
       * up. */
      char err_buf[256];
      snprintf(err_buf, sizeof(err_buf),
               "tool \"%s\" is not registered on this agent", p->name);
      xErrno rc = append_tool_error_(q, p->id, err_buf);
      if (rc != xErrno_Ok) return rc;
      continue;
    }

    /* Confirmation gate: a needs_confirm tool whose host has
     * registered an on_tool_confirm handler gets paused here. The
     * handler is NOT invoked; instead we enqueue an AwaitingConfirm
     * entry and hand the host a resolver. The host eventually calls
     * xAgentToolConfirmResolve() to Allow (resume as inline invoke)
     * or Reject (synthesise is_error=1 tool_result).
     *
     * If on_tool_confirm is NULL the gate is disabled and the tool
     * runs immediately, preserving the legacy behaviour for callers
     * who never wired a confirm handler. */
    if (ai_tool_needs_confirm(t) && q->cbs.on_tool_confirm) {
      struct xAgentToolConfirmResolver_ *r =
        register_confirm_pending_(q, t, p->id, p->name, p->args_json);
      if (!r) return xErrno_NoMemory;
      /* The callback may resolve synchronously (calling
       * xAgentToolConfirmResolve right here). That is safe: the
       * resolver path will transition the entry and, for Allow, run
       * the handler inline \u2014 which may itself complete
       * synchronously, or return Pending and leave the entry as
       * Running. Either way we return to this loop with the array
       * in a consistent state. */
      q->cbs.on_tool_confirm((xAgentQuery)q, p->name, p->id, p->args_json,
                             (xAgentToolConfirmResolver)r, q->cbs.user_data);
      continue;
    }

    xErrno rc = invoke_tool_inline_(q, t, p->id, p->name, p->args_json);
    if (rc != xErrno_Ok) return rc;
  }

  return xErrno_Ok;
}

/* ── Async tool completion ─────────────────────────────────────── */

/* Find an async pending entry by tool_use_id and remove it from the
 * array, returning the entry (caller must free its fields). Returns
 * NULL if not found. */
static struct xAgentQueryAsyncTool_ *async_pending_remove(struct xAgentQuery_ *q,
                                                       const char *tool_use_id) {
  size_t len = xArrayLen(q->async_pending_arr);
  for (size_t i = 0; i < len; i++) {
    struct xAgentQueryAsyncTool_ *a =
      (struct xAgentQueryAsyncTool_ *)xArrayAt(q->async_pending_arr, i);
    if (a->id && strcmp(a->id, tool_use_id) == 0) {
      /* Steal the entry data before removing from array (the remove
       * calls async_pending_release which frees the strings). We
       * make a shallow copy of the struct and NULL out the string
       * pointers so the release callback doesn't free them. */
      struct xAgentQueryAsyncTool_ stolen = *a;
      a->id        = NULL;
      a->name      = NULL;
      a->args_json = NULL;
      a->resolver  = NULL;
      xArrayRemoveRange(q->async_pending_arr, i, 1);
      /* Return the stolen entry; caller owns the strings. */
      struct xAgentQueryAsyncTool_ *out =
        (struct xAgentQueryAsyncTool_ *)calloc(1, sizeof(*out));
      if (out) *out = stolen;
      return out;
    }
  }
  return NULL;
}

/* Called when an async tool completes. Resolves the pending entry,
 * appends the tool_result to produced, and checks whether all async
 * tools are done so the tool-loop can continue. */
void ai_query_async_tool_complete(struct xAgentQuery_ *q,
                                         const char *tool_use_id,
                                         const xAgentContent *result) {
  if (!q || !tool_use_id) return;

  struct xAgentQueryAsyncTool_ *a = async_pending_remove(q, tool_use_id);
  if (!a) return; /* not found — already resolved or stale */

  int         is_error = 0;
  const char *out_text;
  size_t      out_text_len;
  char        err_buf[256];

  if (result && result->type == xAgentContentType_ToolResult) {
    out_text     = result->u.tool_result.output ? result->u.tool_result.output : "";
    out_text_len = result->u.tool_result.output_len
                     ? result->u.tool_result.output_len
                     : strlen(out_text);
    is_error     = result->u.tool_result.is_error ? 1 : 0;
  } else if (result && result->type == xAgentContentType_Text) {
    /* Allow the callback to return a simple text result. */
    out_text     = result->u.text.text ? result->u.text.text : "";
    out_text_len = result->u.text.len ? result->u.text.len : strlen(out_text);
    is_error     = 0;
  } else {
    snprintf(err_buf, sizeof(err_buf),
             "async tool \"%s\" on_done supplied no result", a->name);
    out_text     = err_buf;
    out_text_len = strlen(err_buf);
    is_error     = 1;
  }

  xErrno rc =
    turn_buf_append_tool_result(&q->produced_arr, a->id, out_text,
                                out_text_len, is_error);

  /* Fire on_tool(started=0) now that the async operation is done. */
  if (q->cbs.on_tool) {
    q->cbs.on_tool((xAgentQuery)q, a->name, /*started=*/0, q->cbs.user_data);
  }

  /* Free the stolen entry. */
  free(a->id);
  free(a->name);
  free(a->args_json);
  free(a);

  /* If OOM recording the result, we need to abort the run. */
  if (rc != xErrno_Ok) {
    if (q->cbs.on_error) {
      q->cbs.on_error((xAgentQuery)q, rc,
                      "failed to record async tool_result in history",
                      q->cbs.user_data);
    }
    query_finalize(q, xAgentDoneReason_ToolError);
    return;
  }

  /* If all async tools have completed, continue the tool loop. */
  if (xArrayLen(q->async_pending_arr) == 0) {
    if (q->cancelled) {
      query_finalize(q, xAgentDoneReason_Aborted);
      return;
    }

    /* All async tools resolved — submit the next provider round. */
    xErrno src = submit_round(q);
    if (src != xErrno_Ok) {
      if (q->cbs.on_error) {
        q->cbs.on_error((xAgentQuery)q, src,
                        "failed to submit follow-up after async tools",
                        q->cbs.user_data);
      }
      query_finalize(q, xAgentDoneReason_ModelError);
    }
  }
}

/* ── Assistant-turn commit ─────────────────────────────────────── */

static void commit_assistant_turn(struct xAgentQuery_ *q) {
  /* Order matters on the wire: the thinking block (if any) goes
   * FIRST inside the assistant turn, then the text, then each tool_use
   * entry. moonshot's kimi-k2.6 doesn't appear to care about the
   * exact ordering inside the message, but Anthropic's thinking
   * blocks are documented as coming first, and putting reasoning
   * before tool_calls matches every upstream example I've seen. */
  if (q->reasoning && xBufferLen(q->reasoning) > 0) {
    (void)turn_buf_append_thinking(&q->produced_arr,
                                   (const char *)xBufferData(q->reasoning),
                                   xBufferLen(q->reasoning));
  }
  if (q->assist && xBufferLen(q->assist) > 0) {
    (void)turn_buf_append_text(&q->produced_arr, xAgentRole_Assistant,
                               (const char *)xBufferData(q->assist),
                               xBufferLen(q->assist));
  }
  size_t n_pending = xArrayLen(q->pending_arr);
  for (size_t i = 0; i < n_pending; i++) {
    struct xAgentQueryPending_ *p =
      (struct xAgentQueryPending_ *)xArrayAt(q->pending_arr, i);
    (void)turn_buf_append_tool_use(&q->produced_arr, p->id, p->name,
                                   p->args_json);
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
 *                 to a caller-visible xAgentDoneReason and finish.
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
static xAgentDoneReason translate_terminal(xAgentProviderStopReason r,
                                        int                   user_cancel) {
  if (user_cancel) return xAgentDoneReason_Aborted;
  switch (r) {
  case xAgentProviderStop_EndTurn:
    return xAgentDoneReason_Completed;
  case xAgentProviderStop_MaxTokens:
    return xAgentDoneReason_Completed;
  case xAgentProviderStop_StopSeq:
    return xAgentDoneReason_Stopped;
  case xAgentProviderStop_PromptLong:
    return xAgentDoneReason_PromptTooLong;
  case xAgentProviderStop_Error:
    return xAgentDoneReason_ModelError;
  case xAgentProviderStop_Cancelled:
    return xAgentDoneReason_Aborted;
  case xAgentProviderStop_ToolUse:
    /* Only reached if no pending calls survived (e.g. the model
     * advertised ToolUse but sent zero tool_calls). Treat as a
     * model error rather than silently looping. */
    return xAgentDoneReason_ToolError;
  }
  return xAgentDoneReason_ModelError;
}

/* Finalise the run: snapshot usage, clear the running flag, fire
 * q->cbs.on_done. MUST be the last thing query.c does with @p q in
 * the current call chain — the Session's forwarding on_done will
 * (synchronously, in the same stack) call xAgentQueryDestroy on us and
 * free the memory. No member access past the callback is safe. */
static void query_finalize(struct xAgentQuery_ *q, xAgentDoneReason reason) {
  if (!q->running) return; /* Already finalized — avoid double on_done */

  xAgentUsage usage_snapshot = q->usage;
  int      had_usage      = q->saw_usage;

  q->running = 0;

  if (q->cbs.on_done) {
    q->cbs.on_done((xAgentQuery)q, reason,
                   had_usage ? &usage_snapshot : NULL, q->cbs.user_data);
  }
}

/* Surface transport / model errors to the caller's on_error hook. The
 * run does not end here — the round's assistant output (if any) is
 * still committed and then translated to a terminal done reason. */
static void handle_error(struct xAgentQuery_ *q, xErrno err, const char *msg) {
  if (err != xErrno_Ok && q->cbs.on_error) {
    q->cbs.on_error((xAgentQuery)q, err, msg ? msg : xstrerror(err),
                    q->cbs.user_data);
  }
}

/* Handle the ToolUse branch: honour max_turns, run every buffered
 * handler, and submit the next round. On any failure we finish the
 * run with the appropriate done reason. On success we return and
 * wait for the next on_provider_done callback. */
static void handle_tool_loop_continuation(struct xAgentQuery_ *q) {
  int turn_limit =
    q->max_turns > 0 ? q->max_turns : XAGENT_SESSION_DEFAULT_MAX_TURNS;
  if (q->turn >= turn_limit) {
    /* Already emitted enough rounds; tell the caller we bailed. */
    query_finalize(q, xAgentDoneReason_MaxTurns);
    return;
  }

  /* Run every pending handler; each appends a tool_result entry
   * (synchronous tools) or registers an async_pending entry
   * (tools that returned xErrno_Pending). */
  xErrno drc = dispatch_pending_tools(q);
  pending_reset(q);

  if (q->cancelled) {
    query_finalize(q, xAgentDoneReason_Aborted);
    return;
  }
  if (drc != xErrno_Ok) {
    /* Catastrophic (e.g. OOM appending tool_result). Surface via
     * on_error to give the caller diagnostic detail, then close
     * the run. */
    if (q->cbs.on_error) {
      q->cbs.on_error((xAgentQuery)q, drc,
                      "failed to record tool_result in history",
                      q->cbs.user_data);
    }
    query_finalize(q, xAgentDoneReason_ToolError);
    return;
  }

  /* If some tools are still executing asynchronously, do NOT submit
   * the next round yet. The async completion callback
   * (ai_query_async_tool_complete) will submit the next round once
   * all async tools have resolved. */
  if (xArrayLen(q->async_pending_arr) > 0) {
    return;
  }

  /* Submit the next round. */
  xErrno src = submit_round(q);
  if (src != xErrno_Ok) {
    if (q->cbs.on_error) {
      q->cbs.on_error((xAgentQuery)q, src,
                      "failed to submit follow-up tool round",
                      q->cbs.user_data);
    }
    query_finalize(q, xAgentDoneReason_ModelError);
  }
}

/* Handle the terminal branch: translate the provider's stop reason
 * to a caller-visible done reason and close the run. */
static void handle_terminal(struct xAgentQuery_ *q, xAgentProviderStopReason reason,
                            int user_cancel) {
  query_finalize(q, translate_terminal(reason, user_cancel));
}

/* ── Provider callbacks ────────────────────────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg) {
  struct xAgentQuery_ *q = (struct xAgentQuery_ *)arg;
  if (q->cancelled) return;

  (void)assist_append(q, chunk, len);

  if (q->cbs.on_text) {
    q->cbs.on_text((xAgentQuery)q, chunk, len, q->cbs.user_data);
  }
}

static void on_provider_tool_call(const xAgentContent *call, void *arg) {
  struct xAgentQuery_ *q = (struct xAgentQuery_ *)arg;
  if (q->cancelled || !call || call->type != xAgentContentType_ToolUse) return;
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
  struct xAgentQuery_ *q = (struct xAgentQuery_ *)arg;
  if (q->cancelled) return;

  (void)reasoning_append(q, chunk, len);

  if (q->cbs.on_thinking) {
    q->cbs.on_thinking((xAgentQuery)q, chunk, len, q->cbs.user_data);
  }
}

static void on_provider_done(xAgentProviderStopReason reason, xErrno err,
                             const xAgentUsage *usage, const char *errmsg,
                             void *arg) {
  struct xAgentQuery_ *q = (struct xAgentQuery_ *)arg;

  /* If the query was already finalized (e.g. an async tool's
   * on_cmd_done raced ahead and triggered query_finalize via
   * ai_query_async_tool_complete), bail out — the Query may already
   * have been destroyed by the session's on_done callback. */
  if (!q->running) return;

  /* Fold this round's usage into the running total BEFORE any
   * branching — we want the accounting to be correct whether the
   * run ends here or continues into another tool-loop round. If the
   * provider didn't report usage this round, the accumulator stays
   * where it was. */
  usage_accumulate(q, usage);

  int user_cancel = (reason == xAgentProviderStop_Cancelled) || q->cancelled;

  /* Surface transport / model errors before anything else so the
   * caller's on_error fires in order with the round. */
  if (reason == xAgentProviderStop_Error) {
    handle_error(q, err, errmsg);
  }

  /* Commit the assistant turn into history regardless of outcome —
   * any text or tool_use the model managed to emit is legitimate
   * output. */
  commit_assistant_turn(q);

  /* Continue the tool loop iff: (a) not cancelled, (b) provider said
   * ToolUse AND we buffered >=1 tool call, (c) max_turns not exceeded. */
  int can_continue =
    !user_cancel && reason == xAgentProviderStop_ToolUse && xArrayLen(q->pending_arr) > 0;

  if (can_continue) {
    handle_tool_loop_continuation(q);
    return;
  }

  handle_terminal(q, reason, user_cancel);
}

/* ── Internal API (declared in query_private.h) ────────────────── */

xErrno ai_query_submit(struct xAgentQuery_ *q) {
  return submit_round(q);
}

void ai_query_cancel_mark(struct xAgentQuery_ *q) {
  q->cancelled = 1;
}

void ai_query_take_produced(struct xAgentQuery_ *q, struct xAgentSessionMsg_ **out,
                            size_t *n_out) {
  if (out) *out = (struct xAgentSessionMsg_ *)xArrayData(q->produced_arr);
  if (n_out) *n_out = xArrayLen(q->produced_arr);
  /* Caller must consume before xAgentQueryDestroy, which will release
   * every element via the release callback.  We do NOT clear the
   * array here — the merge path in session.c needs it alive until
   * it has finished copying entries out, and Destroy will clean up
   * whatever remains. */
}

/* ── Public API (declared in xai/query.h) ──────────────────────── */

xAgentQuery xAgentQueryCreate(const xAgentQueryConf *conf) {
  if (!conf) return NULL;
  /* A functional Query requires a provider — without one there is
   * nothing to submit rounds to. The session field is optional
   * (standalone Queries don't need one). */
  if (!conf->provider) return NULL;

  struct xAgentSession_ *s = (struct xAgentSession_ *)conf->session;

  /* Single-flight: at most one live Query per Session today. If
   * conf->session is NULL (standalone Query), no check is needed —
   * the caller manages their own concurrency. The Session also
   * enforces this via xAgentSessionInput's Busy return, but we
   * double-check here so the driving API is safe to call from
   * code paths that don't go through xAgentSessionInput. */
  if (s && s->query) return NULL;

  struct xAgentQuery_ *q = query_alloc();
  if (!q) return NULL;

  /* Self-contained runtime configuration — no Session back-hack. */
  q->provider   = conf->provider;
  q->tools      = conf->tools;
  q->tools_count = conf->tools_count;
  q->model      = conf->model;
  q->max_tokens = conf->max_tokens;
  q->max_turns  = conf->max_turns;
  q->session    = s;         /* observational, never dereferenced for config */
  q->cbs        = conf->cbs;
  usage_reset(q);

  q->inputs_arr   = xArrayCreate(sizeof(struct xAgentSessionMsg_), 8, &kMsgCbs);
  q->produced_arr = xArrayCreate(sizeof(struct xAgentSessionMsg_), 8, &kMsgCbs);
  q->pending_arr  = xArrayCreate(sizeof(struct xAgentQueryPending_), 4,
                                 &kPendingCbs);
  q->async_pending_arr = xArrayCreate(sizeof(struct xAgentQueryAsyncTool_), 4,
                                      &kAsyncPendingCbs);
  if (!q->inputs_arr || !q->produced_arr || !q->pending_arr
      || !q->async_pending_arr) {
    xArrayDestroy(q->inputs_arr);
    xArrayDestroy(q->produced_arr);
    xArrayDestroy(q->pending_arr);
    xArrayDestroy(q->async_pending_arr);
    free(q);
    return NULL;
  }

  s->query = q;
  return (xAgentQuery)q;
}

xErrno xAgentQueryRun(xAgentQuery q, const xAgentMessage *msgs, size_t n) {
  if (!q) return xErrno_InvalidArg;
  if (n == 0 || !msgs) return xErrno_InvalidArg;
  struct xAgentQuery_ *qq = (struct xAgentQuery_ *)q;

  /* Single-shot: a Query that's already running, or has already
   * completed one run, refuses further Run calls. Observable via
   * xAgentQueryIsRunning + "was it Destroyed yet?"; the latter is
   * implicit (we just check running AND turn). */
  if (qq->running || qq->turn > 0) return xErrno_Busy;

  /* Deep-copy every input message into Query-owned turn-entry
   * storage. After this loop succeeds the caller's @p msgs buffers
   * can be freed immediately; everything we need is in qq->inputs. */
  for (size_t i = 0; i < n; i++) {
    xErrno rc = turn_buf_append_message(&qq->inputs_arr, &msgs[i]);
    if (rc != xErrno_Ok) {
      /* Roll back: release anything we managed to copy so the
       * Query is still disposable via xAgentQueryDestroy. */
      xArrayReset(qq->inputs_arr);
      return rc;
    }
  }

  qq->running = 1;

  xErrno rc = submit_round(qq);
  if (rc != xErrno_Ok) {
    /* Round never started — don't fire on_done. Clear running/turn
     * so the Query is back to an idle shape and the caller can
     * safely destroy it. inputs stay populated only for diagnostic
     * purposes — xAgentQueryDestroy will free them. */
    qq->running = 0;
    qq->turn    = 0;
  }
  return rc;
}

void xAgentQueryDestroy(xAgentQuery q) {
  if (!q) return;
  struct xAgentQuery_ *qq = (struct xAgentQuery_ *)q;

  /* If the Query is still running when Destroy is called, we'd need
   * to cancel and await the terminal on_done before freeing. Doing
   * that cleanly requires an async teardown path we don't have yet;
   * for now the Session only calls Destroy from its forwarding
   * on_done (after running has been cleared), so this branch is
   * unreachable in practice. Leave the mark-cancel for when a
   * future caller violates that contract — at least the provider
   * will stop touching the freed arg. */
  if (qq->running) {
    ai_query_cancel_mark(qq);
    ai_provider_cancel(qq->provider);
    /* Cancel in-flight async tools too. */
    for (size_t i = 0; i < xArrayLen(qq->async_pending_arr); i++) {
      struct xAgentQueryAsyncTool_ *a =
        (struct xAgentQueryAsyncTool_ *)xArrayAt(qq->async_pending_arr, i);
      /* AwaitingConfirm entries have no handler running; just
       * invalidate the resolver so any late host callback no-ops. */
      if (a->stage == XAGENT_ASYNC_STAGE_AWAITING_CONFIRM) {
        confirm_resolver_invalidate_(
            (struct xAgentToolConfirmResolver_ *)a->resolver);
        continue;
      }
      if (a->cancel_fn) {
        a->cancel_fn((xAgentQuery)qq, a->id, a->tool, a->cancel_ud);
      }
    }
  }

  /* Detach from Session (if the Session still points at us). */
  if (qq->session && qq->session->query == qq) {
    qq->session->query = NULL;
  }

  xBufferDestroy(qq->assist);
  xBufferDestroy(qq->reasoning);
  xArrayDestroy(qq->pending_arr);
  xArrayDestroy(qq->async_pending_arr);

  xArrayDestroy(qq->inputs_arr);
  xArrayDestroy(qq->produced_arr);

  /* Zero the struct (keeps it clean for the next query_alloc
   * consumer) and return it to the per-thread free list. */
  memset(qq, 0, sizeof(*qq));
  query_free(qq);
}

xAgentQuery xAgentSessionQuery(xAgentSession sess) {
  /* Delegated to session.c — this implementation exists for
   * backward compat with any static-linked callers. */
  if (!sess) return NULL;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;
  return (xAgentQuery)s->query;
}


void xAgentQueryCancel(xAgentQuery q) {
  if (!q) return;
  struct xAgentQuery_ *qq = (struct xAgentQuery_ *)q;
  if (!qq->running) return;

  ai_query_cancel_mark(qq);
  ai_provider_cancel(qq->provider);

  /* Cancel all in-flight async tools. */
  for (size_t i = 0; i < xArrayLen(qq->async_pending_arr); i++) {
    struct xAgentQueryAsyncTool_ *a =
      (struct xAgentQueryAsyncTool_ *)xArrayAt(qq->async_pending_arr, i);
    /* For entries still blocked on user confirmation there is no
     * handler yet to cancel \u2014 just invalidate the resolver so a
     * late xAgentToolConfirmResolve() from the host becomes a no-op. */
    if (a->stage == XAGENT_ASYNC_STAGE_AWAITING_CONFIRM) {
      confirm_resolver_invalidate_(
          (struct xAgentToolConfirmResolver_ *)a->resolver);
      continue;
    }
    if (a->cancel_fn) {
      a->cancel_fn((xAgentQuery)qq, a->id, a->tool, a->cancel_ud);
    }
  }

  /* The provider's on_done will arrive with reason=Cancelled, or
   * dispatch_pending_tools will notice q->cancelled between rounds. */
}

/* Public API: deliver a user decision for a tool-call that was
 * paused on the confirmation gate. See <xagent/query.h>. */
void xAgentToolConfirmResolve(xAgentToolConfirmResolver handle,
                              xAgentToolDecision        decision,
                              const char               *reason) {
  struct xAgentToolConfirmResolver_ *r =
    (struct xAgentToolConfirmResolver_ *)handle;
  if (!r) return;

  /* Second resolve on the same handle: silent no-op. */
  if (r->done) return;
  r->done = 1;

  /* Query already cancelled / destroyed: the entry is gone, the
   * resolver was invalidated. Just free the handle \u2014 the caller
   * won\u2019t get any more use out of it. */
  if (!r->q) {
    confirm_resolver_free_(r);
    return;
  }

  struct xAgentQuery_ *q = r->q;

  /* Locate + remove the AwaitingConfirm entry. This also NULLs out
   * a->resolver on the stolen copy so the release path won\u2019t touch
   * the resolver again. */
  struct xAgentQueryAsyncTool_ *a = async_pending_remove(q, r->id);
  if (!a) {
    /* Entry disappeared between on_tool_confirm and here (e.g. a
     * concurrent cancel). Free the handle and bail. */
    confirm_resolver_free_(r);
    return;
  }

  if (decision == xAgentToolDecision_Reject) {
    /* Fabricate an is_error=1 tool_result and push it through the
     * same completion path an async tool uses. The content gives
     * the model something human-readable to notice. */
    const char *msg = (reason && *reason) ? reason : "rejected by user";
    xAgentContent synth = {0};
    synth.type = xAgentContentType_ToolResult;
    synth.u.tool_result.id         = a->id;
    synth.u.tool_result.output     = msg;
    synth.u.tool_result.output_len = strlen(msg);
    synth.u.tool_result.is_error   = 1;

    /* The async_pending_remove + ai_query_async_tool_complete pairing
     * expects the caller to own the stolen copy; complete() performs
     * a second lookup on the live array. Since we already removed
     * the entry, we have to replicate the tail of complete() manually
     * here \u2014 appending the tool_result and driving the tool-loop
     * continuation if no asyncs remain. */
    xErrno rc = turn_buf_append_tool_result(
        &q->produced_arr, a->id, msg, synth.u.tool_result.output_len,
        /*is_error=*/1);

    free(a->id);
    free(a->name);
    free(a->args_json);
    free(a);
    confirm_resolver_free_(r);

    if (rc != xErrno_Ok) {
      if (q->cbs.on_error) {
        q->cbs.on_error((xAgentQuery)q, rc,
                        "failed to record rejected tool_result",
                        q->cbs.user_data);
      }
      query_finalize(q, xAgentDoneReason_ToolError);
      return;
    }

    /* If that was the last outstanding async / confirm entry, the
     * tool-loop can submit the next round. Otherwise keep waiting. */
    if (xArrayLen(q->async_pending_arr) == 0) {
      if (q->cancelled) {
        query_finalize(q, xAgentDoneReason_Aborted);
        return;
      }
      xErrno src = submit_round(q);
      if (src != xErrno_Ok) {
        if (q->cbs.on_error) {
          q->cbs.on_error((xAgentQuery)q, src,
                          "failed to submit follow-up after rejected tool",
                          q->cbs.user_data);
        }
        query_finalize(q, xAgentDoneReason_ModelError);
      }
    }
    return;
  }

  /* Allow path: transition to Running by invoking the handler
   * inline. We\u2019ve already removed the AwaitingConfirm entry from
   * async_pending_arr, so invoke_tool_inline_ is free to push a new
   * Running entry if the handler returns Pending.
   *
   * Free the stolen entry and the resolver now; invoke owns the
   * follow-up bookkeeping. */
  xAgentTool  t         = a->tool;
  char       *id_copy   = a->id;    /* move ownership of strings into locals */
  char       *name_copy = a->name;
  char       *args_copy = a->args_json;
  free(a);
  confirm_resolver_free_(r);

  xErrno rc = invoke_tool_inline_(q, t, id_copy, name_copy, args_copy);
  free(id_copy);
  free(name_copy);
  free(args_copy);

  if (rc != xErrno_Ok) {
    if (q->cbs.on_error) {
      q->cbs.on_error((xAgentQuery)q, rc,
                      "failed to invoke confirmed tool", q->cbs.user_data);
    }
    query_finalize(q, xAgentDoneReason_ToolError);
    return;
  }

  /* If no async/confirm entries are outstanding (all remaining
   * tools were synchronous), we must drive the loop forward just
   * like the Reject branch does. */
  if (xArrayLen(q->async_pending_arr) == 0) {
    if (q->cancelled) {
      query_finalize(q, xAgentDoneReason_Aborted);
      return;
    }
    xErrno src = submit_round(q);
    if (src != xErrno_Ok) {
      if (q->cbs.on_error) {
        q->cbs.on_error((xAgentQuery)q, src,
                        "failed to submit follow-up after confirmed tool",
                        q->cbs.user_data);
      }
      query_finalize(q, xAgentDoneReason_ModelError);
    }
  }
}

int xAgentQueryIsRunning(xAgentQuery q) {
  if (!q) return 0;
  return ((struct xAgentQuery_ *)q)->running;
}

void xAgentQueryUsage(xAgentQuery q, xAgentUsage *out) {
  if (!out) return;
  if (!q) {
    out->prompt_tokens     = -1;
    out->completion_tokens = -1;
    out->total_tokens      = -1;
    return;
  }
  *out = ((struct xAgentQuery_ *)q)->usage;
}

xAgentSession xAgentQuerySession(xAgentQuery q) {
  if (!q) return NULL;
  return (xAgentSession)((struct xAgentQuery_ *)q)->session;
}

int xAgentQueryTurn(xAgentQuery q) {
  if (!q) return 0;
  return ((struct xAgentQuery_ *)q)->turn;
}
