/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query.c - xAiQuery implementation
 *
 * One Query is one end-to-end run of the provider / tool loop:
 *
 *     Create → Run(msgs[]) → round 1 → (tools?) → round 2 → ... → on_done
 *
 * Session owns the durable state (conversation history,
 * configuration, caller callbacks). Each accepted xAiSessionInput()
 * heap-allocates a fresh Query via xAiQueryCreate, hands it the
 * full message list it should run on via xAiQueryRun, and destroys
 * it from its forwarding on_done once the terminal callback has
 * fired. Query owns the transient state of that single run (input
 * snapshot, produced turns, streaming buffers, pending tool calls,
 * cumulative usage, running/cancelled flags) and fires its own
 * @c q->cbs.* stream — the Session installs forwarding shims that
 * re-dispatch those calls to the caller's xAiSessionCallbacks.
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
 * xAiQuerySession() API and the single-flight guard.
 */

#include "query_private.h"
#include "session_private.h"

#include "agent_private.h"
#include "provider_private.h" /* ai_provider_submit / ai_provider_cancel   */
#include "tool_private.h"     /* ai_tool_name / ai_tool_invoke             */

#include <xai/message.h>
#include <xai/provider.h>
#include <xai/query.h>
#include <xai/session.h>
#include <xai/tool.h>
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
 * xAiQueryCreate / Destroy are called on every Session input cycle,
 * making them a hot path.  A per-thread free list avoids
 * malloc/free overhead entirely — zero locks, zero atomics.
 *
 * We reuse query->next as the freelist link pointer (zero extra
 * memory). A per-thread cap prevents unbounded caching.
 */
#define QUERY_FREELIST_CAP 16

struct query_freelist {
  struct xAiQuery_ *head;
  size_t            count;
};

static __thread struct query_freelist tl_qfree = {NULL, 0};

static struct xAiQuery_ *query_alloc(void) {
  if (tl_qfree.head) {
    struct xAiQuery_ *q = tl_qfree.head;
    tl_qfree.head       = q->next;
    tl_qfree.count--;
    q->next = NULL;
    return q;
  }
  return (struct xAiQuery_ *)calloc(1, sizeof(struct xAiQuery_));
}

static void query_free(struct xAiQuery_ *q) {
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
 * Both inputs_arr and produced_arr store struct xAiSessionMsg_
 * elements whose heap-owned strings must be freed on removal.
 * The release callback wraps ai_session_msg_free(); no retain
 * callback is needed because callers fill zero-initialised slots
 * manually after xArrayPush(). */

static void query_msg_release(void *elem) {
  ai_session_msg_free((struct xAiSessionMsg_ *)elem);
}

static const xArrayCallbacks kMsgCbs = { NULL, query_msg_release, NULL };

/* Push a new zero-initialised slot onto a turn-entry array. */
static struct xAiSessionMsg_ *msg_push(xArray *arrp) {
  return (struct xAiSessionMsg_ *)xArrayPush(arrp);
}

/* Append a single-content text entry with role @p role. The text is
 * duplicated. Used to materialise user text and assistant text into
 * turn-entry storage. */
static xErrno turn_buf_append_text(xArray *arrp, xAiRole role,
                                   const char *text, size_t len) {
  struct xAiSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
  slot->kind = xAiSessionEntry_Text;
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
  struct xAiSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Assistant;
  slot->kind = xAiSessionEntry_Thinking;
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
  struct xAiSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role          = xAiRole_Assistant;
  slot->kind          = xAiSessionEntry_ToolUse;
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
  struct xAiSessionMsg_ *slot = msg_push(arrp);
  if (!slot) return xErrno_NoMemory;
  slot->role           = xAiRole_Tool;
  slot->kind           = xAiSessionEntry_ToolResult;
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

/* Deep-copy one incoming xAiMessage into @p buf as one or more
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
static xErrno turn_buf_append_message(xArray *arrp, const xAiMessage *msg) {
  if (!msg) return xErrno_InvalidArg;

  if (msg->role == xAiRole_Assistant) {
    for (size_t i = 0; i < msg->n; i++) {
      const xAiContent *c = &msg->contents[i];
      xErrno            rc;
      switch (c->type) {
      case xAiContentType_Text:
        rc = turn_buf_append_text(arrp, xAiRole_Assistant,
                                  c->u.text.text, c->u.text.len);
        break;
      case xAiContentType_Thinking:
        rc = turn_buf_append_thinking(arrp, c->u.thinking.text,
                                      c->u.thinking.len);
        break;
      case xAiContentType_ToolUse:
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

  if (msg->role == xAiRole_Tool) {
    for (size_t i = 0; i < msg->n; i++) {
      const xAiContent *c = &msg->contents[i];
      if (c->type != xAiContentType_ToolResult) continue;
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
    if (msg->contents[i].type == xAiContentType_Text) {
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
    if (msg->contents[i].type == xAiContentType_Text) {
      size_t k = msg->contents[i].u.text.len;
      if (k) memcpy(concat + off, msg->contents[i].u.text.text, k);
      off += k;
    }
  }
  concat[total]               = '\0';
  struct xAiSessionMsg_ *slot = msg_push(arrp);
  if (!slot) {
    free(concat);
    return xErrno_NoMemory;
  }
  slot->role     = msg->role;
  slot->kind     = xAiSessionEntry_Text;
  slot->text     = concat;
  slot->text_len = total;
  return xErrno_Ok;
}

/* ── Pending tool-call bookkeeping ──────────────────────────────── */

static void pending_release(void *elem) {
  struct xAiQueryPending_ *p = (struct xAiQueryPending_ *)elem;
  if (!p) return;
  free(p->id);
  free(p->name);
  free(p->args_json);
  memset(p, 0, sizeof(*p));
}

static const xArrayCallbacks kPendingCbs = { NULL, pending_release, NULL };

static void pending_reset(struct xAiQuery_ *q) {
  xArrayReset(q->pending_arr);
}

/* Append one pending tool_call. Copies every string. */
static xErrno pending_append(struct xAiQuery_ *q, const xAiContent *call) {
  struct xAiQueryPending_ *slot =
    (struct xAiQueryPending_ *)xArrayPush(&q->pending_arr);
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
static xErrno assist_append(struct xAiQuery_ *q, const char *chunk,
                            size_t len) {
  if (len == 0) return xErrno_Ok;
  if (!q->assist) {
    q->assist = xBufferCreate(256);
    if (!q->assist) return xErrno_NoMemory;
  }
  return xBufferAppend(&q->assist, chunk, len);
}

static void assist_reset(struct xAiQuery_ *q) {
  if (q->assist) xBufferReset(q->assist);
}

/* Same shape as assist_append, but for the current-round reasoning /
 * thinking stream. Kept separate so the two never race to the same
 * buffer and so view_build can emit them as distinct content blocks
 * on the wire. */
static xErrno reasoning_append(struct xAiQuery_ *q, const char *chunk,
                               size_t len) {
  if (len == 0) return xErrno_Ok;
  if (!q->reasoning) {
    q->reasoning = xBufferCreate(256);
    if (!q->reasoning) return xErrno_NoMemory;
  }
  return xBufferAppend(&q->reasoning, chunk, len);
}

static void reasoning_reset(struct xAiQuery_ *q) {
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
static void usage_accumulate(struct xAiQuery_ *q, const xAiUsage *round) {
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
#define XAI_FOLD_MAX(field)                                                 \
  do {                                                                       \
    if (round->field >= 0) {                                                 \
      q->usage.field = (q->usage.field < 0) ? round->field                  \
                         : (q->usage.field > round->field                    \
                              ? q->usage.field                               \
                              : round->field);                               \
    }                                                                        \
  } while (0)

#define XAI_FOLD_ADD(field)                                                 \
  do {                                                                       \
    if (round->field >= 0) {                                                 \
      q->usage.field =                                                       \
        (q->usage.field < 0) ? round->field : q->usage.field + round->field; \
    }                                                                        \
  } while (0)

  XAI_FOLD_MAX(prompt_tokens);
  XAI_FOLD_ADD(completion_tokens);
  XAI_FOLD_ADD(total_tokens);

#undef XAI_FOLD_MAX
#undef XAI_FOLD_ADD

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

static void usage_reset(struct xAiQuery_ *q) {
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
 * carrying thinking / text / tool_use blocks together); every other
 * entry maps 1:1 to an xAiMessage.
 *
 * The entry sequence the builder walks is @c q->inputs concatenated
 * with @c q->produced: inputs carries whatever the Session handed
 * to xAiQueryRun (System prompt + rolling history + the new user
 * turn), produced carries everything this run has emitted so far
 * (assistant turns + tool_results from previous rounds). The Query
 * never reads @c s->history; session.c is responsible for baking
 * the system prompt into @c inputs before Run. */
static xErrno view_build(struct xAiQuery_ *q, struct view_ *out) {
  memset(out, 0, sizeof(*out));

  /* We present inputs and produced as one logical sequence; the
   * two arrays make the fold loop below simpler than a runtime
   * concat. */
  struct xAiSessionMsg_ *arrs[2] = {
    (struct xAiSessionMsg_ *)xArrayData(q->inputs_arr),
    (struct xAiSessionMsg_ *)xArrayData(q->produced_arr)
  };
  size_t lens[2] = { xArrayLen(q->inputs_arr), xArrayLen(q->produced_arr) };
  size_t n_total = lens[0] + lens[1];
  if (n_total == 0) return xErrno_InvalidArg;

  /* Helper: address of logical entry i (across arrs[0]++arrs[1]). */
#define XAI_ENTRY(i) \
  (((i) < lens[0]) ? &arrs[0][(i)] : &arrs[1][(i) - lens[0]])

  /* Pass 1: count output messages and content blocks. */
  size_t n_msgs   = 0;
  size_t n_blocks = 0;
  for (size_t i = 0; i < n_total;) {
    struct xAiSessionMsg_ *m = XAI_ENTRY(i);
    if (m->role == xAiRole_Assistant) {
      size_t j = i;
      while (j < n_total && XAI_ENTRY(j)->role == xAiRole_Assistant) j++;
      n_msgs += 1;
      n_blocks += (j - i);
      i = j;
    } else {
      n_msgs += 1;
      n_blocks += 1;
      i += 1;
    }
  }

  out->msgs   = (xAiMessage *)calloc(n_msgs, sizeof(xAiMessage));
  out->blocks = (xAiContent *)calloc(n_blocks, sizeof(xAiContent));
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
    struct xAiSessionMsg_ *m = XAI_ENTRY(i);
    if (m->role == xAiRole_Assistant) {
      size_t block_start = bi;
      size_t j           = i;
      while (j < n_total && XAI_ENTRY(j)->role == xAiRole_Assistant) {
        struct xAiSessionMsg_ *mm = XAI_ENTRY(j);
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

#undef XAI_ENTRY
  return xErrno_Ok;
}

/* ── Provider callbacks (forward decls) ───────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg);
static void on_provider_tool_call(const xAiContent *call, void *arg);
static void on_provider_thinking(const char *chunk, size_t len, void *arg);
static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             const xAiUsage *usage, const char *errmsg,
                             void *arg);

/* ── Submit ────────────────────────────────────────────────────── */

static xErrno submit_round(struct xAiQuery_ *q) {
  struct view_ v;
  xErrno       rc = view_build(q, &v);
  if (rc != xErrno_Ok) return rc;

  xAiProviderSubmitConf pc = {0};
  pc.model                 = q->model;
  pc.messages              = v.msgs;
  pc.n_messages            = v.n_msgs;
  pc.tools                 = q->tools;
  pc.tools_count           = q->tools_count;
  pc.temperature           = -1;
  pc.max_tokens            = q->max_tokens;
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

  rc = ai_provider_submit(q->provider, &pc, &cbs, q);
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
static xAiTool find_tool(const xAiTool **tools, size_t tools_count,
                        const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < tools_count; i++) {
    if (!tools[i]) continue;
    xAiTool     t = *tools[i];
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
 * (OOM building result entries) aborts with the returned code. */
static xErrno dispatch_pending_tools(struct xAiQuery_ *q) {
  for (size_t i = 0; i < xArrayLen(q->pending_arr) && !q->cancelled; i++) {
    struct xAiQueryPending_ *p =
      (struct xAiQueryPending_ *)xArrayAt(q->pending_arr, i);

    if (q->cbs.on_tool) {
      q->cbs.on_tool((xAiQuery)q, p->name, /*started=*/1, q->cbs.user_data);
    }

    xAiTool     t        = find_tool(q->tools, q->tools_count, p->name);
    xAiContent  out      = {0};
    int         is_error = 0;
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
      xAiContent in           = {0};
      in.type                 = xAiContentType_ToolUse;
      in.u.tool_use.id        = p->id;
      in.u.tool_use.name      = p->name;
      in.u.tool_use.args_json = p->args_json;
      xErrno trc              = ai_tool_invoke(t, &in, &out);
      if (trc != xErrno_Ok) {
        snprintf(err_buf, sizeof(err_buf),
                 "tool handler returned error (xErrno=%d)", (int)trc);
        out_text     = err_buf;
        out_text_len = strlen(err_buf);
        is_error     = 1;
      } else if (out.type == xAiContentType_ToolResult) {
        out_text     = out.u.tool_result.output ? out.u.tool_result.output : "";
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

    xErrno rc =
      turn_buf_append_tool_result(&q->produced_arr, p->id, out_text,
                                  out_text_len, is_error);

    if (q->cbs.on_tool) {
      q->cbs.on_tool((xAiQuery)q, p->name, /*started=*/0, q->cbs.user_data);
    }

    if (rc != xErrno_Ok) return rc;
  }

  return xErrno_Ok;
}

/* ── Assistant-turn commit ─────────────────────────────────────── */

static void commit_assistant_turn(struct xAiQuery_ *q) {
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
    (void)turn_buf_append_text(&q->produced_arr, xAiRole_Assistant,
                               (const char *)xBufferData(q->assist),
                               xBufferLen(q->assist));
  }
  size_t n_pending = xArrayLen(q->pending_arr);
  for (size_t i = 0; i < n_pending; i++) {
    struct xAiQueryPending_ *p =
      (struct xAiQueryPending_ *)xArrayAt(q->pending_arr, i);
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
                                        int                   user_cancel) {
  if (user_cancel) return xAiDoneReason_Aborted;
  switch (r) {
  case xAiProviderStop_EndTurn:
    return xAiDoneReason_Completed;
  case xAiProviderStop_MaxTokens:
    return xAiDoneReason_Completed;
  case xAiProviderStop_StopSeq:
    return xAiDoneReason_Stopped;
  case xAiProviderStop_PromptLong:
    return xAiDoneReason_PromptTooLong;
  case xAiProviderStop_Error:
    return xAiDoneReason_ModelError;
  case xAiProviderStop_Cancelled:
    return xAiDoneReason_Aborted;
  case xAiProviderStop_ToolUse:
    /* Only reached if no pending calls survived (e.g. the model
     * advertised ToolUse but sent zero tool_calls). Treat as a
     * model error rather than silently looping. */
    return xAiDoneReason_ToolError;
  }
  return xAiDoneReason_ModelError;
}

/* Finalise the run: snapshot usage, clear the running flag, fire
 * q->cbs.on_done. MUST be the last thing query.c does with @p q in
 * the current call chain — the Session's forwarding on_done will
 * (synchronously, in the same stack) call xAiQueryDestroy on us and
 * free the memory. No member access past the callback is safe. */
static void query_finalize(struct xAiQuery_ *q, xAiDoneReason reason) {
  xAiUsage usage_snapshot = q->usage;
  int      had_usage      = q->saw_usage;

  q->running = 0;

  if (q->cbs.on_done) {
    q->cbs.on_done((xAiQuery)q, reason,
                   had_usage ? &usage_snapshot : NULL, q->cbs.user_data);
  }
}

/* Surface transport / model errors to the caller's on_error hook. The
 * run does not end here — the round's assistant output (if any) is
 * still committed and then translated to a terminal done reason. */
static void handle_error(struct xAiQuery_ *q, xErrno err, const char *msg) {
  if (err != xErrno_Ok && q->cbs.on_error) {
    q->cbs.on_error((xAiQuery)q, err, msg ? msg : xstrerror(err),
                    q->cbs.user_data);
  }
}

/* Handle the ToolUse branch: honour max_turns, run every buffered
 * handler, and submit the next round. On any failure we finish the
 * run with the appropriate done reason. On success we return and
 * wait for the next on_provider_done callback. */
static void handle_tool_loop_continuation(struct xAiQuery_ *q) {
  int turn_limit =
    q->max_turns > 0 ? q->max_turns : XAI_SESSION_DEFAULT_MAX_TURNS;
  if (q->turn >= turn_limit) {
    /* Already emitted enough rounds; tell the caller we bailed. */
    query_finalize(q, xAiDoneReason_MaxTurns);
    return;
  }

  /* Run every pending handler; each appends a tool_result entry. */
  xErrno drc = dispatch_pending_tools(q);
  pending_reset(q);

  if (q->cancelled) {
    query_finalize(q, xAiDoneReason_Aborted);
    return;
  }
  if (drc != xErrno_Ok) {
    /* Catastrophic (e.g. OOM appending tool_result). Surface via
     * on_error to give the caller diagnostic detail, then close
     * the run. */
    if (q->cbs.on_error) {
      q->cbs.on_error((xAiQuery)q, drc,
                      "failed to record tool_result in history",
                      q->cbs.user_data);
    }
    query_finalize(q, xAiDoneReason_ToolError);
    return;
  }

  /* Submit the next round. */
  xErrno src = submit_round(q);
  if (src != xErrno_Ok) {
    if (q->cbs.on_error) {
      q->cbs.on_error((xAiQuery)q, src,
                      "failed to submit follow-up tool round",
                      q->cbs.user_data);
    }
    query_finalize(q, xAiDoneReason_ModelError);
  }
}

/* Handle the terminal branch: translate the provider's stop reason
 * to a caller-visible done reason and close the run. */
static void handle_terminal(struct xAiQuery_ *q, xAiProviderStopReason reason,
                            int user_cancel) {
  query_finalize(q, translate_terminal(reason, user_cancel));
}

/* ── Provider callbacks ────────────────────────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg) {
  struct xAiQuery_ *q = (struct xAiQuery_ *)arg;
  if (q->cancelled) return;

  (void)assist_append(q, chunk, len);

  if (q->cbs.on_text) {
    q->cbs.on_text((xAiQuery)q, chunk, len, q->cbs.user_data);
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

  if (q->cbs.on_thinking) {
    q->cbs.on_thinking((xAiQuery)q, chunk, len, q->cbs.user_data);
  }
}

static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             const xAiUsage *usage, const char *errmsg,
                             void *arg) {
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
    handle_error(q, err, errmsg);
  }

  /* Commit the assistant turn into history regardless of outcome —
   * any text or tool_use the model managed to emit is legitimate
   * output. */
  commit_assistant_turn(q);

  /* Continue the tool loop iff: (a) not cancelled, (b) provider said
   * ToolUse AND we buffered >=1 tool call, (c) max_turns not exceeded. */
  int can_continue =
    !user_cancel && reason == xAiProviderStop_ToolUse && xArrayLen(q->pending_arr) > 0;

  if (can_continue) {
    handle_tool_loop_continuation(q);
    return;
  }

  handle_terminal(q, reason, user_cancel);
}

/* ── Internal API (declared in query_private.h) ────────────────── */

xErrno ai_query_submit(struct xAiQuery_ *q) {
  return submit_round(q);
}

void ai_query_cancel_mark(struct xAiQuery_ *q) {
  q->cancelled = 1;
}

void ai_query_take_produced(struct xAiQuery_ *q, struct xAiSessionMsg_ **out,
                            size_t *n_out) {
  if (out) *out = (struct xAiSessionMsg_ *)xArrayData(q->produced_arr);
  if (n_out) *n_out = xArrayLen(q->produced_arr);
  /* Caller must consume before xAiQueryDestroy, which will release
   * every element via the release callback.  We do NOT clear the
   * array here — the merge path in session.c needs it alive until
   * it has finished copying entries out, and Destroy will clean up
   * whatever remains. */
}

/* ── Public API (declared in xai/query.h) ──────────────────────── */

xAiQuery xAiQueryCreate(const xAiQueryConf *conf) {
  if (!conf) return NULL;
  /* A functional Query requires a provider — without one there is
   * nothing to submit rounds to. The session field is optional
   * (standalone Queries don't need one). */
  if (!conf->provider) return NULL;

  struct xAiSession_ *s = (struct xAiSession_ *)conf->session;

  /* Single-flight: at most one live Query per Session today. If
   * conf->session is NULL (standalone Query), no check is needed —
   * the caller manages their own concurrency. The Session also
   * enforces this via xAiSessionInput's Busy return, but we
   * double-check here so the driving API is safe to call from
   * code paths that don't go through xAiSessionInput. */
  if (s && s->query) return NULL;

  struct xAiQuery_ *q = query_alloc();
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

  q->inputs_arr   = xArrayCreate(sizeof(struct xAiSessionMsg_), 8, &kMsgCbs);
  q->produced_arr = xArrayCreate(sizeof(struct xAiSessionMsg_), 8, &kMsgCbs);
  q->pending_arr  = xArrayCreate(sizeof(struct xAiQueryPending_), 4,
                                 &kPendingCbs);
  if (!q->inputs_arr || !q->produced_arr || !q->pending_arr) {
    xArrayDestroy(q->inputs_arr);
    xArrayDestroy(q->produced_arr);
    xArrayDestroy(q->pending_arr);
    free(q);
    return NULL;
  }

  s->query = q;
  return (xAiQuery)q;
}

xErrno xAiQueryRun(xAiQuery q, const xAiMessage *msgs, size_t n) {
  if (!q) return xErrno_InvalidArg;
  if (n == 0 || !msgs) return xErrno_InvalidArg;
  struct xAiQuery_ *qq = (struct xAiQuery_ *)q;

  /* Single-shot: a Query that's already running, or has already
   * completed one run, refuses further Run calls. Observable via
   * xAiQueryIsRunning + "was it Destroyed yet?"; the latter is
   * implicit (we just check running AND turn). */
  if (qq->running || qq->turn > 0) return xErrno_Busy;

  /* Deep-copy every input message into Query-owned turn-entry
   * storage. After this loop succeeds the caller's @p msgs buffers
   * can be freed immediately; everything we need is in qq->inputs. */
  for (size_t i = 0; i < n; i++) {
    xErrno rc = turn_buf_append_message(&qq->inputs_arr, &msgs[i]);
    if (rc != xErrno_Ok) {
      /* Roll back: release anything we managed to copy so the
       * Query is still disposable via xAiQueryDestroy. */
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
     * purposes — xAiQueryDestroy will free them. */
    qq->running = 0;
    qq->turn    = 0;
  }
  return rc;
}

void xAiQueryDestroy(xAiQuery q) {
  if (!q) return;
  struct xAiQuery_ *qq = (struct xAiQuery_ *)q;

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
  }

  /* Detach from Session (if the Session still points at us). */
  if (qq->session && qq->session->query == qq) {
    qq->session->query = NULL;
  }

  xBufferDestroy(qq->assist);
  xBufferDestroy(qq->reasoning);
  xArrayDestroy(qq->pending_arr);

  xArrayDestroy(qq->inputs_arr);
  xArrayDestroy(qq->produced_arr);

  /* Zero the struct (keeps it clean for the next query_alloc
   * consumer) and return it to the per-thread free list. */
  memset(qq, 0, sizeof(*qq));
  query_free(qq);
}

xAiQuery xAiSessionQuery(xAiSession sess) {
  /* Delegated to session.c — this implementation exists for
   * backward compat with any static-linked callers. */
  if (!sess) return NULL;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  return (xAiQuery)s->query;
}


void xAiQueryCancel(xAiQuery q) {
  if (!q) return;
  struct xAiQuery_ *qq = (struct xAiQuery_ *)q;
  if (!qq->running) return;

  ai_query_cancel_mark(qq);
  ai_provider_cancel(qq->provider);
  /* The provider's on_done will arrive with reason=Cancelled, or
   * dispatch_pending_tools will notice q->cancelled between rounds. */
}

int xAiQueryIsRunning(xAiQuery q) {
  if (!q) return 0;
  return ((struct xAiQuery_ *)q)->running;
}

void xAiQueryUsage(xAiQuery q, xAiUsage *out) {
  if (!out) return;
  if (!q) {
    out->prompt_tokens     = -1;
    out->completion_tokens = -1;
    out->total_tokens      = -1;
    return;
  }
  *out = ((struct xAiQuery_ *)q)->usage;
}

xAiSession xAiQuerySession(xAiQuery q) {
  if (!q) return NULL;
  return (xAiSession)((struct xAiQuery_ *)q)->session;
}

int xAiQueryTurn(xAiQuery q) {
  if (!q) return 0;
  return ((struct xAiQuery_ *)q)->turn;
}

