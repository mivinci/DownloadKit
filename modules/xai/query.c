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
 * This file reaches back into its owning Session via @c q->session
 * only for agent-level configuration (provider, tools, model,
 * max_tokens, max_turns) — everything conversation-shaped is
 * Query-local.
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

/* ── Turn-entry buffer helpers (local to query.c) ───────────────────
 *
 * Both @c q->inputs and @c q->produced are flat arrays of
 * struct xAiSessionMsg_ (same shape as xAiSession_::history; see
 * turn_private.h for the rationale). These helpers own all their
 * strings and release them through ai_session_msg_free(), which is
 * shared with session.c.
 *
 * We keep the helpers local rather than promoting them to
 * session_private.h: session.c has its own grow/append pair tuned
 * for xAiSession_::history, and query.c only needs this handful for
 * the two Query-local arrays. Sharing would save maybe 15 lines
 * at the cost of cross-TU coupling we are actively trying to
 * reduce. If a third buffer shows up we'll revisit.
 */
static struct xAiSessionMsg_ *turn_buf_grow(struct xAiSessionMsg_ **buf,
                                            size_t *n, size_t *cap) {
  if (*n + 1 > *cap) {
    size_t new_cap = *cap ? *cap * 2 : 4;
    struct xAiSessionMsg_ *nb = (struct xAiSessionMsg_ *)realloc(
      *buf, new_cap * sizeof(**buf));
    if (!nb) return NULL;
    *buf = nb;
    *cap = new_cap;
  }
  struct xAiSessionMsg_ *slot = &(*buf)[(*n)++];
  memset(slot, 0, sizeof(*slot));
  return slot;
}

static void turn_buf_free(struct xAiSessionMsg_ **buf, size_t *n, size_t *cap) {
  if (*buf) {
    for (size_t i = 0; i < *n; i++) ai_session_msg_free(&(*buf)[i]);
    free(*buf);
  }
  *buf = NULL;
  *n   = 0;
  *cap = 0;
}

/* Append a single-content text entry with role @p role. The text is
 * duplicated. Used to materialise user text and assistant text into
 * turn-entry storage. */
static xErrno turn_buf_append_text(struct xAiSessionMsg_ **buf, size_t *n,
                                   size_t *cap, xAiRole role, const char *text,
                                   size_t len) {
  struct xAiSessionMsg_ *slot = turn_buf_grow(buf, n, cap);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
  slot->kind = xAiSessionEntry_Text;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      (*n)--;
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

/* Append an assistant thinking entry. @p text is duplicated. */
static xErrno turn_buf_append_thinking(struct xAiSessionMsg_ **buf, size_t *n,
                                       size_t *cap, const char *text,
                                       size_t len) {
  struct xAiSessionMsg_ *slot = turn_buf_grow(buf, n, cap);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Assistant;
  slot->kind = xAiSessionEntry_Thinking;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      (*n)--;
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

/* Append an assistant tool_use entry. Every string is duplicated. */
static xErrno turn_buf_append_tool_use(struct xAiSessionMsg_ **buf, size_t *n,
                                       size_t *cap, const char *id,
                                       const char *name, const char *args) {
  struct xAiSessionMsg_ *slot = turn_buf_grow(buf, n, cap);
  if (!slot) return xErrno_NoMemory;
  slot->role          = xAiRole_Assistant;
  slot->kind          = xAiSessionEntry_ToolUse;
  slot->tool_use_id   = dup_cstr(id ? id : "");
  slot->tool_use_name = dup_cstr(name ? name : "");
  slot->tool_use_args = dup_cstr(args ? args : "{}");
  if (!slot->tool_use_id || !slot->tool_use_name || !slot->tool_use_args) {
    ai_session_msg_free(slot);
    (*n)--;
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Append a tool_result entry. The output is duplicated. */
static xErrno turn_buf_append_tool_result(struct xAiSessionMsg_ **buf,
                                          size_t *n, size_t *cap,
                                          const char *id, const char *output,
                                          size_t output_len, int is_error) {
  struct xAiSessionMsg_ *slot = turn_buf_grow(buf, n, cap);
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
    ai_session_msg_free(slot);
    (*n)--;
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
static xErrno turn_buf_append_message(struct xAiSessionMsg_ **buf, size_t *n,
                                      size_t *cap, const xAiMessage *msg) {
  if (!msg) return xErrno_InvalidArg;

  if (msg->role == xAiRole_Assistant) {
    for (size_t i = 0; i < msg->n; i++) {
      const xAiContent *c = &msg->contents[i];
      xErrno            rc;
      switch (c->type) {
      case xAiContentType_Text:
        rc = turn_buf_append_text(buf, n, cap, xAiRole_Assistant,
                                  c->u.text.text, c->u.text.len);
        break;
      case xAiContentType_Thinking:
        rc = turn_buf_append_thinking(buf, n, cap, c->u.thinking.text,
                                      c->u.thinking.len);
        break;
      case xAiContentType_ToolUse:
        rc = turn_buf_append_tool_use(buf, n, cap, c->u.tool_use.id,
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
        buf, n, cap, c->u.tool_result.id, c->u.tool_result.output,
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
    return turn_buf_append_text(buf, n, cap, msg->role, NULL, 0);
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
  struct xAiSessionMsg_ *slot = turn_buf_grow(buf, n, cap);
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
    size_t                   new_cap = q->cap_pending ? q->cap_pending * 2 : 4;
    struct xAiQueryPending_ *np =
      (struct xAiQueryPending_ *)realloc(q->pending, new_cap * sizeof(*np));
    if (!np) return xErrno_NoMemory;
    q->pending     = np;
    q->cap_pending = new_cap;
  }
  struct xAiQueryPending_ *slot = &q->pending[q->n_pending];
  memset(slot, 0, sizeof(*slot));
  slot->id   = dup_cstr(call->u.tool_use.id ? call->u.tool_use.id : "");
  slot->name = dup_cstr(call->u.tool_use.name ? call->u.tool_use.name : "");
  slot->args_json =
    dup_cstr(call->u.tool_use.args_json ? call->u.tool_use.args_json : "{}");
  if (!slot->id || !slot->name || !slot->args_json) {
    pending_free_entry(slot);
    return xErrno_NoMemory;
  }
  q->n_pending++;
  return xErrno_Ok;
}

/* ── Round-scoped streaming accumulators ───────────────────────── */

/* Append one byte range to the current-round assistant text buffer. */
static xErrno assist_append(struct xAiQuery_ *q, const char *chunk,
                            size_t len) {
  if (len == 0) return xErrno_Ok;
  if (q->assist_len + len + 1 > q->assist_cap) {
    size_t new_cap = q->assist_cap ? q->assist_cap : 256;
    while (new_cap < q->assist_len + len + 1)
      new_cap *= 2;
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
    while (new_cap < q->reasoning_len + len + 1)
      new_cap *= 2;
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

#define XAI_FOLD(field)                                                      \
  do {                                                                       \
    if (round->field >= 0) {                                                 \
      q->usage.field =                                                       \
        (q->usage.field < 0) ? round->field : q->usage.field + round->field; \
    }                                                                        \
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
   * two dimension pairs make the fold loop below simpler than a
   * runtime concat. */
  struct xAiSessionMsg_ *arrs[2] = {q->inputs, q->produced};
  size_t                 lens[2] = {q->n_inputs, q->n_produced};
  size_t                 n_total = lens[0] + lens[1];
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
                             const xAiUsage *usage, void *arg);

/* ── Submit ────────────────────────────────────────────────────── */

static xErrno submit_round(struct xAiQuery_ *q) {
  struct xAiSession_ *s = q->session;
  struct xAiAgent_   *a = (struct xAiAgent_ *)s->agent;

  struct view_ v;
  xErrno       rc = view_build(q, &v);
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
    xAiTool     t = *a->tools[i];
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
  struct xAiSession_ *s = q->session;
  struct xAiAgent_   *a = (struct xAiAgent_ *)s->agent;

  for (size_t i = 0; i < q->n_pending && !q->cancelled; i++) {
    struct xAiQueryPending_ *p = &q->pending[i];

    if (q->cbs.on_tool) {
      q->cbs.on_tool((xAiQuery)q, p->name, /*started=*/1, q->cbs.user_data);
    }

    xAiTool     t        = find_tool(a, p->name);
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
      turn_buf_append_tool_result(&q->produced, &q->n_produced,
                                  &q->cap_produced, p->id, out_text,
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
  if (q->reasoning_len > 0) {
    (void)turn_buf_append_thinking(&q->produced, &q->n_produced,
                                   &q->cap_produced, q->reasoning_buf,
                                   q->reasoning_len);
  }
  if (q->assist_len > 0) {
    (void)turn_buf_append_text(&q->produced, &q->n_produced,
                               &q->cap_produced, xAiRole_Assistant,
                               q->assist_buf, q->assist_len);
  }
  for (size_t i = 0; i < q->n_pending; i++) {
    struct xAiQueryPending_ *p = &q->pending[i];
    (void)turn_buf_append_tool_use(&q->produced, &q->n_produced,
                                   &q->cap_produced, p->id, p->name,
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
static void handle_error(struct xAiQuery_ *q, xErrno err) {
  if (err != xErrno_Ok && q->cbs.on_error) {
    q->cbs.on_error((xAiQuery)q, err, NULL, q->cbs.user_data);
  }
}

/* Handle the ToolUse branch: honour max_turns, run every buffered
 * handler, and submit the next round. On any failure we finish the
 * run with the appropriate done reason. On success we return and
 * wait for the next on_provider_done callback. */
static void handle_tool_loop_continuation(struct xAiQuery_ *q) {
  struct xAiSession_ *s = q->session;
  int                 turn_limit =
    s->max_turns > 0 ? s->max_turns : XAI_SESSION_DEFAULT_MAX_TURNS;
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
  int can_continue =
    !user_cancel && reason == xAiProviderStop_ToolUse && q->n_pending > 0;

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
  if (out) *out = q->produced;
  if (n_out) *n_out = q->n_produced;
  q->produced     = NULL;
  q->n_produced   = 0;
  q->cap_produced = 0;
}

/* ── Public API (declared in xai/query.h) ──────────────────────── */

xAiQuery xAiQueryCreate(xAiSession sess, const xAiQueryConf *conf) {
  if (!sess || !conf) return NULL;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  /* Single-flight: at most one live Query per Session today. The
   * Session also enforces this via xAiSessionInput's Busy return,
   * but we double-check here so the driving API is safe to call
   * from code paths that don't go through xAiSessionInput. */
  if (s->query) return NULL;

  struct xAiQuery_ *q = (struct xAiQuery_ *)calloc(1, sizeof(*q));
  if (!q) return NULL;

  q->session = s;
  q->cbs     = conf->cbs;
  usage_reset(q);
  /* running / cancelled / turn and all buffers start zero from calloc. */

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

  struct xAiSession_ *s = qq->session;
  if (!s) return xErrno_InvalidArg;

  /* Deep-copy every input message into Query-owned turn-entry
   * storage. After this loop succeeds the caller's @p msgs buffers
   * can be freed immediately; everything we need is in qq->inputs. */
  for (size_t i = 0; i < n; i++) {
    xErrno rc = turn_buf_append_message(&qq->inputs, &qq->n_inputs,
                                        &qq->cap_inputs, &msgs[i]);
    if (rc != xErrno_Ok) {
      /* Roll back: release anything we managed to copy so the
       * Query is still disposable via xAiQueryDestroy. */
      turn_buf_free(&qq->inputs, &qq->n_inputs, &qq->cap_inputs);
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
    struct xAiSession_ *s = qq->session;
    if (s) {
      struct xAiAgent_ *a = (struct xAiAgent_ *)s->agent;
      if (a) ai_provider_cancel(a->provider);
    }
  }

  /* Detach from Session (if the Session still points at us). */
  if (qq->session && qq->session->query == qq) {
    qq->session->query = NULL;
  }

  free(qq->assist_buf);
  free(qq->reasoning_buf);
  pending_reset(qq);
  free(qq->pending);

  turn_buf_free(&qq->inputs, &qq->n_inputs, &qq->cap_inputs);
  turn_buf_free(&qq->produced, &qq->n_produced, &qq->cap_produced);

  free(qq);
}

xAiQuery xAiSessionQuery(xAiSession sess) {
  if (!sess) return NULL;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  return (xAiQuery)s->query;
}

void xAiQueryCancel(xAiQuery q) {
  if (!q) return;
  struct xAiQuery_ *qq = (struct xAiQuery_ *)q;
  if (!qq->running) return;

  ai_query_cancel_mark(qq);

  struct xAiSession_ *s = qq->session;
  if (s) {
    struct xAiAgent_ *a = (struct xAiAgent_ *)s->agent;
    if (a) ai_provider_cancel(a->provider);
  }
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

