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
 *   - Full tool loop: collect tool_use deltas during a round, dispatch
 *     to registered handlers on the event loop when the provider
 *     reports Stop_ToolUse, stitch tool_result entries into history,
 *     and submit the next round — up to the configured max_turns.
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

/* Fallback cap if neither the caller nor the agent set max_turns.
 * Picked to be generous enough for typical multi-step tool use while
 * still catching runaway agent loops. */
#define XAI_SESSION_DEFAULT_MAX_TURNS 16

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

static void pending_free_entry(struct xAiSessionPending_ *p) {
  if (!p) return;
  free(p->id);
  free(p->name);
  free(p->args_json);
  memset(p, 0, sizeof(*p));
}

static void pending_reset(struct xAiSession_ *s) {
  for (size_t i = 0; i < s->n_pending; i++) {
    pending_free_entry(&s->pending[i]);
  }
  s->n_pending = 0;
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

/* Append one text entry to the history, copying every byte. */
static xErrno history_append_text(struct xAiSession_ *s, xAiRole role,
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

/* Append an Assistant tool_use entry. */
static xErrno history_append_tool_use(struct xAiSession_ *s, const char *id,
                                      const char *name, const char *args) {
  struct xAiSessionMsg_ *slot = history_grow(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Assistant;
  slot->kind = xAiSessionEntry_ToolUse;
  slot->tool_use_id   = dup_cstr(id ? id : "");
  slot->tool_use_name = dup_cstr(name ? name : "");
  slot->tool_use_args = dup_cstr(args ? args : "{}");
  if (!slot->tool_use_id || !slot->tool_use_name || !slot->tool_use_args) {
    msg_free(slot);
    s->n_history--;
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Append an Assistant thinking / reasoning entry. Payload reuses the
 * Text-kind `text` / `text_len` slot so msg_free() stays uniform. */
static xErrno history_append_thinking(struct xAiSession_ *s, const char *text,
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

/* Append a Tool tool_result entry. */
static xErrno history_append_tool_result(struct xAiSession_ *s, const char *id,
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
    msg_free(slot);
    s->n_history--;
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Append an incoming xAiMessage (caller-supplied, shallow view) into
 * our history. Every text block is concatenated into a single text
 * entry; non-text blocks on the user side are ignored. */
static xErrno history_append_user_msg(struct xAiSession_ *s, xAiMessage msg) {
  size_t total = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAiContentType_Text) {
      total += msg.contents[i].u.text.len;
    }
  }
  if (total == 0) {
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
  slot->kind     = xAiSessionEntry_Text;
  slot->text     = buf;
  slot->text_len = total;
  return xErrno_Ok;
}

/* Append one byte range to the current-round assistant text buffer. */
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

/* Same shape as assist_append, but for the current-round reasoning /
 * thinking stream. Kept separate so the two never race to the same
 * buffer and so view_build can emit them as distinct content blocks
 * on the wire. */
static xErrno reasoning_append(struct xAiSession_ *s, const char *chunk,
                               size_t len) {
  if (len == 0) return xErrno_Ok;
  if (s->reasoning_len + len + 1 > s->reasoning_cap) {
    size_t new_cap = s->reasoning_cap ? s->reasoning_cap : 256;
    while (new_cap < s->reasoning_len + len + 1) new_cap *= 2;
    char *nb = (char *)realloc(s->reasoning_buf, new_cap);
    if (!nb) return xErrno_NoMemory;
    s->reasoning_buf = nb;
    s->reasoning_cap = new_cap;
  }
  memcpy(s->reasoning_buf + s->reasoning_len, chunk, len);
  s->reasoning_len += len;
  s->reasoning_buf[s->reasoning_len] = '\0';
  return xErrno_Ok;
}

static void reasoning_reset(struct xAiSession_ *s) {
  s->reasoning_len = 0;
  if (s->reasoning_buf) s->reasoning_buf[0] = '\0';
}

/* Append one pending tool_call. Copies every string. */
static xErrno pending_append(struct xAiSession_ *s, const xAiContent *call) {
  if (s->n_pending + 1 > s->cap_pending) {
    size_t new_cap = s->cap_pending ? s->cap_pending * 2 : 4;
    struct xAiSessionPending_ *np = (struct xAiSessionPending_ *)realloc(
        s->pending, new_cap * sizeof(*np));
    if (!np) return xErrno_NoMemory;
    s->pending     = np;
    s->cap_pending = new_cap;
  }
  struct xAiSessionPending_ *slot = &s->pending[s->n_pending];
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
  s->n_pending++;
  return xErrno_Ok;
}

/* ── Submit-view construction ──────────────────────────────────────── */

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
static xErrno view_build(struct xAiSession_ *s, struct view_ *out) {
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

/* ── Round submission ──────────────────────────────────────────────── */

/* Forward decls for the callback dispatch. */
static void on_provider_text(const char *chunk, size_t len, void *arg);
static void on_provider_tool_call(const xAiContent *call, void *arg);
static void on_provider_thinking(const char *chunk, size_t len, void *arg);
static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             const xAiUsage *usage, void *arg);

/* Fold one round's usage into the session-wide running total.
 *
 * Semantics: -1 is the "unknown" sentinel on both sides. If the
 * round contributes a real number for a given field and the running
 * total is still -1, we replace (first real report wins the initial
 * value). If both are real numbers, we add. If the round's field is
 * -1, we leave the running total alone. Anthropic-style
 * cache_creation / cache_read will slot in the same way once we
 * teach the provider to parse them. */
static void usage_accumulate(struct xAiSession_ *s, const xAiUsage *round) {
  if (!round) return;
  s->saw_usage = 1;

#define XAI_FOLD(field)                                                       \
  do {                                                                        \
    if (round->field >= 0) {                                                  \
      s->usage.field = (s->usage.field < 0) ? round->field                    \
                                            : s->usage.field + round->field;  \
    }                                                                         \
  } while (0)

  XAI_FOLD(prompt_tokens);
  XAI_FOLD(completion_tokens);
  XAI_FOLD(total_tokens);

#undef XAI_FOLD
}

static void usage_reset(struct xAiSession_ *s) {
  s->saw_usage               = 0;
  s->usage.prompt_tokens     = -1;
  s->usage.completion_tokens = -1;
  s->usage.total_tokens      = -1;
}

/* Submit a new provider round over the current history.
 *
 * Precondition: s->running == 1. Caller is responsible for appending
 * the user message / tool_results into history before calling.
 *
 * Returns xErrno_Ok if the submit was accepted (on_done will fire
 * later). On failure the caller must decide how to unwind. */
static xErrno submit_round(struct xAiSession_ *s) {
  struct xAiAgent_ *a = (struct xAiAgent_ *)s->agent;

  struct view_ v;
  xErrno rc = view_build(s, &v);
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

  assist_reset(s);
  reasoning_reset(s);
  pending_reset(s);
  s->turn++;

  rc = ai_provider_submit(a->provider, &pc, &cbs, s);
  view_free(&v);
  return rc;
}

/* Finish the current session run and fire on_done. After this
 * returns, the session is idle. */
static void finish_run(struct xAiSession_ *s, xAiDoneReason reason) {
  /* Snapshot usage before reset: the callback sees the running
   * totals we accumulated across every provider round, or NULL if
   * nothing ever reported. */
  xAiUsage        usage_snapshot = s->usage;
  int             had_usage      = s->saw_usage;

  assist_reset(s);
  reasoning_reset(s);
  pending_reset(s);
  usage_reset(s);
  s->running   = 0;
  s->cancelled = 0;
  s->turn      = 0;
  if (s->cbs.on_done) {
    s->cbs.on_done((xAiSession)s, reason,
                   had_usage ? &usage_snapshot : NULL,
                   s->cbs.user_data);
  }
}

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
 * history. If the session is cancelled mid-dispatch we stop early and
 * let finish_run surface Aborted.
 *
 * Returns xErrno_Ok if dispatch completed (successfully or with
 * individual tool errors folded back into history). Fatal failure
 * (OOM building result entries) aborts with the returned code. */
static xErrno dispatch_pending_tools(struct xAiSession_ *s) {
  struct xAiAgent_ *a = (struct xAiAgent_ *)s->agent;

  for (size_t i = 0; i < s->n_pending && !s->cancelled; i++) {
    struct xAiSessionPending_ *p = &s->pending[i];

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

    xErrno rc = history_append_tool_result(s, p->id, out_text, out_text_len,
                                           is_error);

    if (s->cbs.on_tool) {
      s->cbs.on_tool((xAiSession)s, p->name, /*started=*/0, s->cbs.user_data);
    }

    if (rc != xErrno_Ok) return rc;
  }

  return xErrno_Ok;
}

/* ── Provider callbacks ─────────────────────────────────────────────── */

static void on_provider_text(const char *chunk, size_t len, void *arg) {
  struct xAiSession_ *s = (struct xAiSession_ *)arg;
  if (s->cancelled) return;

  (void)assist_append(s, chunk, len);

  if (s->cbs.on_text) {
    s->cbs.on_text((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

static void on_provider_tool_call(const xAiContent *call, void *arg) {
  struct xAiSession_ *s = (struct xAiSession_ *)arg;
  if (s->cancelled || !call || call->type != xAiContentType_ToolUse) return;
  /* Buffer; actual dispatch happens from on_provider_done when we
   * know the assistant message is complete. */
  (void)pending_append(s, call);
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
  struct xAiSession_ *s = (struct xAiSession_ *)arg;
  if (s->cancelled) return;

  (void)reasoning_append(s, chunk, len);

  if (s->cbs.on_thinking) {
    s->cbs.on_thinking((xAiSession)s, chunk, len, s->cbs.user_data);
  }
}

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

static void commit_assistant_turn(struct xAiSession_ *s) {
  /* Order matters on the wire: the thinking block (if any) goes
   * FIRST inside the assistant turn, then the text, then each tool_use
   * entry. moonshot's kimi-k2.6 doesn't appear to care about the
   * exact ordering inside the message, but Anthropic's thinking
   * blocks are documented as coming first, and putting reasoning
   * before tool_calls matches every upstream example I've seen. */
  if (s->reasoning_len > 0) {
    (void)history_append_thinking(s, s->reasoning_buf, s->reasoning_len);
  }
  if (s->assist_len > 0) {
    (void)history_append_text(s, xAiRole_Assistant, s->assist_buf,
                              s->assist_len);
  }
  for (size_t i = 0; i < s->n_pending; i++) {
    struct xAiSessionPending_ *p = &s->pending[i];
    (void)history_append_tool_use(s, p->id, p->name, p->args_json);
  }
}

static void on_provider_done(xAiProviderStopReason reason, xErrno err,
                             const xAiUsage *usage, void *arg) {
  struct xAiSession_ *s = (struct xAiSession_ *)arg;

  /* Fold this round's usage into the running total BEFORE any
   * branching — we want the accounting to be correct whether the
   * run ends here or continues into another tool-loop round. If the
   * provider didn't report usage this round, the accumulator stays
   * where it was. */
  usage_accumulate(s, usage);

  int user_cancel = (reason == xAiProviderStop_Cancelled) || s->cancelled;

  /* Surface transport / model errors before anything else so the
   * caller's on_error fires in order with the round. */
  if (reason == xAiProviderStop_Error && err != xErrno_Ok && s->cbs.on_error) {
    s->cbs.on_error((xAiSession)s, err, NULL, s->cbs.user_data);
  }

  /* Commit the assistant turn into history regardless of outcome —
   * any text or tool_use the model managed to emit is legitimate
   * output. */
  commit_assistant_turn(s);

  /* Continue the tool loop iff: (a) not cancelled, (b) provider said
   * ToolUse AND we buffered >=1 tool call, (c) max_turns not exceeded. */
  int can_continue = !user_cancel && reason == xAiProviderStop_ToolUse &&
                     s->n_pending > 0;

  if (can_continue) {
    int turn_limit = s->max_turns > 0 ? s->max_turns
                                      : XAI_SESSION_DEFAULT_MAX_TURNS;
    if (s->turn >= turn_limit) {
      /* Already emitted enough rounds; tell the caller we bailed. */
      finish_run(s, xAiDoneReason_MaxTurns);
      return;
    }

    /* Run every pending handler; each appends a tool_result entry. */
    xErrno drc = dispatch_pending_tools(s);
    pending_reset(s);

    if (s->cancelled) {
      finish_run(s, xAiDoneReason_Aborted);
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
      finish_run(s, xAiDoneReason_ToolError);
      return;
    }

    /* Submit the next round. */
    xErrno src = submit_round(s);
    if (src != xErrno_Ok) {
      if (s->cbs.on_error) {
        s->cbs.on_error((xAiSession)s, src,
                        "failed to submit follow-up tool round",
                        s->cbs.user_data);
      }
      finish_run(s, xAiDoneReason_ModelError);
    }
    return;
  }

  finish_run(s, translate_terminal(reason, user_cancel));
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

  /* Usage is unknown until a round reports it. -1 is the sentinel;
   * calloc zeroed us to 0 which would lie. */
  usage_reset(s);

  return (xAiSession)s;
}

xErrno xAiSessionInput(xAiSession sess, xAiMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  if (s->running) return xErrno_Busy;

  size_t history_checkpoint = s->n_history;
  xErrno rc = history_append_user_msg(s, msg);
  if (rc != xErrno_Ok) return rc;

  /* Flip running BEFORE submit: submit may deliver callbacks
   * synchronously, and our on_provider_done clears running. */
  s->running   = 1;
  s->cancelled = 0;
  s->turn      = 0;
  usage_reset(s);

  rc = submit_round(s);
  if (rc != xErrno_Ok) {
    /* Round never started — don't fire on_done. Undo the user
     * message we appended so the history tracks what actually
     * happened. */
    s->running = 0;
    s->turn    = 0;
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
  /* on_provider_done will arrive with reason=Cancelled (or we are
   * between rounds and dispatch_pending_tools will notice). */
}

void xAiSessionDestroy(xAiSession sess) {
  if (!sess) return;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  if (s->running) {
    xAiSessionCancel(sess);
  }

  for (size_t i = 0; i < s->n_history; i++) msg_free(&s->history[i]);
  free(s->history);
  free(s->assist_buf);
  free(s->reasoning_buf);
  pending_reset(s);
  free(s->pending);
  free(s);
}
