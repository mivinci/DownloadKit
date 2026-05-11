/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session.c - xAgentSession implementation
 *
 * Scope:
 *   - Lifecycle (create / destroy / cancel).
 *   - Single-flight admission with xErrno_Busy.
 *   - History ownership: every byte (user text, assistant text,
 *     tool_use arguments, tool_result output) is duplicated into
 *     session-owned storage. Callers can drop their xAgentMessage as
 *     soon as xAgentSessionInput returns.
 *   - Query lifecycle: each accepted xAgentSessionInput builds the
 *     complete message list the model should see (System prompt +
 *     rolling history + the new user turn), creates a fresh Query
 *     via xAgentQueryCreate, runs it with xAgentQueryRun, and destroys
 *     it from a forwarding on_done once the terminal callback has
 *     fired. Anything the Query produced during the run is pulled
 *     out via ai_query_take_produced() and merged back into the
 *     Session's history before the Query is released.
 *   - Context-budget gate: before appending the user turn and
 *     creating a Query, xAgentSessionInput runs the incoming msg +
 *     current history through the budget estimator (budget.c) and
 *     dispatches on xAgentBudgetConf::policy. Error refuses with
 *     xErrno_PromptTooLong; TruncateTail (formerly TruncateOldest)
 *     drops history entries from the tail end (cache-friendly
 *     direction that preserves the prompt prefix for provider-side
 *     prompt caching), and, if that still does not fit, refuses.
 *     Disabled (the default) is a single-branch short-circuit and
 *     leaves behaviour byte-identical to pre-budget releases.
 *     Callback / Summarize policies are accepted by the
 *     parser but behave like Error until c4+ wires the real
 *     implementations.
 *   - Incremental token bookkeeping: instead of re-estimating the
 *     full history every turn and correcting with an EWMA
 *     calibrator, we use provider-reported prompt_tokens as the
 *     precise baseline and only estimate the delta (new entries
 *     since the last provider report). This gives much higher
 *     accuracy with simpler code.
 *
 * The provider / tool loop itself lives in query.c; the Session
 * installs a static set of forwarding callbacks that re-dispatch
 * Query-level events (on_text, on_thinking, on_done, ...) to the
 * caller's xAgentSessionCallbacks with the Session handle. The Query
 * runs off an explicit message array and never reads or writes
 * s->history directly — that is the boundary this split enforces.
 *
 * Intentionally still deferred (see libs/xai/TODO.md):
 *   - Parallel tool dispatch via xTaskGroup when concurrent_safe is
 *     set. Today every handler runs synchronously on the loop thread.
 *   - User-confirmation gate for needs_confirm tools.
 *   - Budget-policy implementations for Callback (caller-supplied
 *     compaction) and Summarize (async summary query).
 *     Multi-round tool runs do not yet contribute calibration
 *     observations; a proper split of per-round usage out of
 *     Query would unlock that in a later commit.
 *   - Proper async teardown when destroy is called mid-flight.
 */

#include "session_private.h"

#include "agent_private.h"
#include "budget_private.h"
#include "tool_private.h"

#include <xagent/memory.h>
#include <xagent/message.h>
#include <xagent/provider.h>
#include <xagent/query.h>
#include <xagent/session.h>
#include <xbase/array.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/time.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ──────────────────────────────────────────── */

static void sess_fwd_on_done(xAgentQuery q, xAgentDoneReason reason,
                             const xAgentUsage *usage, void *ud);
static void session_sidecar_idle_timer_cb(void *arg);
static void session_sidecar_on_done(xAgentQuery q, xAgentDoneReason reason,
                                    const xAgentUsage *usage, void *ud);
static void session_sidecar_cleanup(struct xAgentSession_ *s);

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

void ai_session_msg_free(struct xAgentSessionMsg_ *m) {
  if (!m) return;
  free(m->text);
  free(m->tool_use_id);
  free(m->tool_use_name);
  free(m->tool_use_args);
  free(m->tool_result_id);
  free(m->tool_result_output);
  memset(m, 0, sizeof(*m));
}

/* xArray release callback for xAgentSessionMsg_ elements. */
static void session_msg_release(void *elem) {
  ai_session_msg_free((struct xAgentSessionMsg_ *)elem);
}

/* xArray callbacks for session history: release only (no retain, no equal).
 * Push returns a zero-initialised slot; the caller fills it manually. */
static const xArrayCallbacks kHistoryCbs = {NULL, session_msg_release, NULL};

/* Push a new zero-initialised slot onto the history array.
 * Returns the slot pointer, or NULL on allocation failure. The
 * returned slot has @c created_at_ms pre-filled with the current
 * wall-clock time; callers that are copying an already-stamped
 * record (produced → history splice) will overwrite the whole
 * struct and therefore carry the earlier, more accurate stamp. */
static struct xAgentSessionMsg_ *history_push(struct xAgentSession_ *s) {
  struct xAgentSessionMsg_ *slot =
    (struct xAgentSessionMsg_ *)xArrayPush(&s->history_arr);
  if (slot) slot->created_at_ms = xWallMs();
  return slot;
}

/* ── History append API (shared with query.c via session_private.h) ── */

xErrno ai_history_append_text(struct xAgentSession_ *s, xAgentRole role,
                              const char *text, size_t len, int is_summary) {
  struct xAgentSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role       = role;
  slot->kind       = xAgentSessionEntry_Text;
  slot->is_summary = is_summary;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      xArrayPop(s->history_arr);
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

xErrno ai_history_append_tool_use(struct xAgentSession_ *s, const char *id,
                                  const char *name, const char *args) {
  struct xAgentSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role          = xAgentRole_Assistant;
  slot->kind          = xAgentSessionEntry_ToolUse;
  slot->tool_use_id   = dup_cstr(id ? id : "");
  slot->tool_use_name = dup_cstr(name ? name : "");
  slot->tool_use_args = dup_cstr(args ? args : "{}");
  if (!slot->tool_use_id || !slot->tool_use_name || !slot->tool_use_args) {
    /* xArrayPop calls session_msg_release which frees the partial
     * fields we just allocated. */
    xArrayPop(s->history_arr);
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Payload reuses the Text-kind @c text / @c text_len slot so
 * ai_session_msg_free() stays uniform. */
xErrno ai_history_append_thinking(struct xAgentSession_ *s, const char *text,
                                  size_t len) {
  struct xAgentSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAgentRole_Assistant;
  slot->kind = xAgentSessionEntry_Thinking;
  if (len > 0) {
    slot->text = dup_bytes(text, len);
    if (!slot->text) {
      xArrayPop(s->history_arr);
      return xErrno_NoMemory;
    }
    slot->text_len = len;
  }
  return xErrno_Ok;
}

xErrno ai_history_append_tool_result(struct xAgentSession_ *s, const char *id,
                                     const char *output, size_t output_len,
                                     int is_error) {
  struct xAgentSessionMsg_ *slot = history_push(s);
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
    /* xArrayPop calls session_msg_release which frees the partial
     * fields we just allocated. */
    xArrayPop(s->history_arr);
    return xErrno_NoMemory;
  }
  return xErrno_Ok;
}

/* Concatenate every text block of an incoming user-role xAgentMessage
 * into a single text entry and append it to history. Non-text blocks
 * are ignored (user-side tool_result entries come in via the tool
 * pipeline, not xAgentSessionInput). Kept static: only xAgentSessionInput
 * consumes this shape now that the Query no longer drives history
 * writes itself. */
static xErrno history_append_user_msg(struct xAgentSession_ *s,
                                      xAgentMessage          msg) {
  size_t total = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAgentContentType_Text) {
      total += msg.contents[i].u.text.len;
    }
  }
  if (total == 0) {
    return ai_history_append_text(s, msg.role, NULL, 0, 0);
  }

  char *buf = (char *)malloc(total + 1);
  if (!buf) return xErrno_NoMemory;
  size_t off = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAgentContentType_Text) {
      size_t n = msg.contents[i].u.text.len;
      if (n) memcpy(buf + off, msg.contents[i].u.text.text, n);
      off += n;
    }
  }
  buf[total] = '\0';

  struct xAgentSessionMsg_ *slot = history_push(s);
  if (!slot) {
    free(buf);
    return xErrno_NoMemory;
  }
  slot->role     = msg.role;
  slot->kind     = xAgentSessionEntry_Text;
  slot->text     = buf;
  slot->text_len = total;
  return xErrno_Ok;
}

/* ── Context-budget enforcement ─────────────────────────────────────
 *
 * Gate xAgentSessionInput runs on the structured xAgentBudgetConf policy.
 * The pipeline runs BEFORE the user message is committed to
 * history, so the Error policy can refuse the turn without leaving
 * history polluted.
 *
 * Wire picture:
 *
 *   xAgentSessionInput(sess, msg)
 *     │
 *     ├─ budget.policy == Disabled   → skip, proceed as before
 *     ├─ budget.policy == Error      → estimate; if over, return
 *     │                                xErrno_PromptTooLong
 *     └─ Summarize             → estimate; if over, retroactively
 *                                      trim tool_results, then launch
 *                                      an internal compact Query.
 *                                      On compact success the old
 *                                      history is replaced by a summary
 *                                      and the pending input is
 *                                      auto-retried. On compact failure
 *                                      history is left untouched and
 *                                      the caller is notified via
 *                                      xAgentBudgetEvent_CompactDone.
 *
 * The estimator is the coarse one from budget.c — bytes/4 plus a
 * per-entry envelope constant — and is intentionally conservative
 * so a slightly-overestimate still errs on the side of "refuse /
 * trim a bit more" rather than busting the real provider window. */

/* Historically the session had a session_trim_history_front_() helper
 * that wiped history[0..keep_idx) in place. It was removed when the
 * Summarize pipeline moved to front-replacing compaction: instead
 * of preserving the head and splicing the summary into the middle,
 * the new pipeline replaces history[0..compact_end_idx) with a
 * summary at index 0 via xArrayRemoveRange + xArrayInsert directly
 * in sess_fwd_on_done. persisted_prefix adjustment is inlined at
 * the call site (see the "compact_ok" branch of sess_fwd_on_done). */

/* ── Incoming user message token estimator ────────────────────
 *
 * Coarse token estimate for an incoming user message so the
 * budget gate can decide whether to let it through before
 * the message is committed to history. */
static size_t estimate_incoming_user_tokens_(xAgentMessage msg) {
  size_t payload_bytes = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAgentContentType_Text) {
      payload_bytes += msg.contents[i].u.text.len;
    }
  }
  return (payload_bytes / XAGENT_BUDGET_BYTES_PER_TOKEN) +
         XAGENT_BUDGET_PER_MSG_TOKENS;
}

/* ── Retroactive tool_result trimming ───────────────────────────
 *
 * When context usage exceeds the budget limit, this function
 * scans all history entries and truncates "consumed" tool_result
 * outputs. A tool_result is "consumed" when the model has already
 * seen it and generated a subsequent response — i.e. there exists
 * an Assistant-role entry after it in history.
 *
 * The output is replaced with a short marker like
 *   "[result trimmed: was 12345 bytes]"
 * freeing token budget while keeping the tool_use/tool_result pair
 * structurally intact (the model still knows it called the tool and
 * got a result — it just doesn't see the full output anymore).
 *
 * This is lighter-weight than compacting: no entire turns are
 * removed, the conversation flow is preserved.
 *
 * Returns the number of tool_result entries that were trimmed.
 */
static size_t session_trim_consumed_tool_results_(struct xAgentSession_ *s,
                                                  ssize_t *out_bytes_freed) {
  size_t  trimmed     = 0;
  ssize_t bytes_freed = 0;

  /* Scan all history entries, trimming consumed tool_result
   * outputs. A tool_result is "consumed" when the model has
   * already seen it and generated a subsequent response — i.e.
   * there exists an Assistant-role entry after it in history.
   * We scan from end to start so we process the oldest entries
   * first.
   *
   * The output is replaced with a short marker like
   *   "[result trimmed: was 12345 bytes]"
   * freeing token budget while keeping the tool_use/tool_result
   * pair structurally intact (the model still knows it called the
   * tool and got a result — it just doesn't see the full output
   * anymore). */
  size_t hlen = xArrayLen(s->history_arr);
  for (size_t i = hlen; i > 0; i--) {
    struct xAgentSessionMsg_ *e =
      &((struct xAgentSessionMsg_ *)xArrayData(s->history_arr))[i - 1];

    if (e->kind == xAgentSessionEntry_ToolResult &&
        e->tool_result_output_len > 0 && e->tool_result_output != NULL &&
        /* Skip entries that were already trimmed — their output
         * starts with "[result trimmed:". Re-trimming them wastes
         * a compact round and falsely reports "N entries trimmed"
         * with 0 bytes freed, which can trigger an infinite loop
         * when the Summarize path re-enters after seeing the
         * non-zero trimmed count. */
        strncmp(e->tool_result_output,
                "[result trimmed: ", strlen("[result trimmed: ")) != 0) {
      size_t old_bytes = e->tool_result_output_len;

      static const char kTrimSuffix[] = "[result trimmed: was %zu bytes]";
      char              marker[96];
      size_t            marker_len =
        (size_t)snprintf(marker, sizeof(marker), kTrimSuffix, old_bytes);

      char *buf = (char *)malloc(marker_len + 1);
      if (!buf) continue; /* skip on alloc failure */
      memcpy(buf, marker, marker_len);
      buf[marker_len] = '\0';

      free(e->tool_result_output);
      e->tool_result_output     = buf;
      e->tool_result_output_len = marker_len;

      bytes_freed += (ssize_t)old_bytes - (ssize_t)marker_len;
      trimmed++;
    }
  }

  if (trimmed > 0) {
    /* Invalidate known_prompt_tokens — history content changed. */
    s->known_prompt_tokens = -1;
    s->delta_entries       = 0;
  }

  if (out_bytes_freed) *out_bytes_freed = bytes_freed;
  return trimmed;
}

/* Resolve the effective token ceiling for this session. A zero
 * @c max_tokens in the budget conf means "use the built-in
 * default"; callers that want a tighter or looser cap MUST set an
 * explicit value. Kept inline-ish so each policy branch does not
 * repeat the fallback. */
static size_t session_budget_limit_(const struct xAgentSession_ *s) {
  return s->budget.context_window > 0 ? s->budget.context_window
                                      : XAGENT_BUDGET_DEFAULT_MAX_TOKENS;
}

/* Decide whether this incoming user message, combined with the
 * current history, fits in the configured budget — and if not,
 * act according to @c budget.policy. May mutate @c s->history
 * (trim path).
 *
 * Returns xErrno_Ok to mean "proceed: it's safe to append and run
 * a Query"; any other value short-circuits xAgentSessionInput.
 *
 * Note: the system_prompt is intentionally NOT counted here. It
 * is a borrowed, fixed-size sidecar that the trimmer cannot touch
 * anyway (invariant 1), so excluding it from the budget math keeps
 * the "can I trim enough to fit?" question honest: including it
 * would make an over-large system prompt look like regular history
 * pressure and yield no-op trims.
 *
 * Incremental bookkeeping: when known_prompt_tokens >= 0 we use
 * the provider-reported value as the precise baseline and only
 * estimate the delta (new entries since last report). When
 * known_prompt_tokens < 0 (cold start or post-trim invalidation)
 * we fall back to estimating the full history with the coarse
 * bytes/4 heuristic. */

/* Compute the current estimated token count using incremental
 * bookkeeping when available, or the coarse full-history estimate
 * as fallback. */
static size_t session_estimate_current_(struct xAgentSession_ *s) {
  if (s->known_prompt_tokens >= 0 && s->delta_entries > 0) {
    /* Incremental: precise baseline + coarse delta estimate. */
    size_t hist_len = xArrayLen(s->history_arr);
    size_t delta_start =
      hist_len > s->delta_entries ? hist_len - s->delta_entries : 0;
    size_t delta_est = ai_budget_estimate_tokens(
      (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr) +
        delta_start,
      s->delta_entries);
    return (size_t)s->known_prompt_tokens + delta_est;
  }
  if (s->known_prompt_tokens >= 0 && s->delta_entries == 0) {
    /* No new entries since last provider report — the known value
     * is still precise. */
    return (size_t)s->known_prompt_tokens;
  }
  /* Cold start or post-trim invalidation: fall back to full
   * coarse estimate. */
  return ai_budget_estimate_tokens(
    (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
    xArrayLen(s->history_arr));
}

/* ── Building the Query input from Session state ──────────────────── *
 *
 * Every xAgentSessionInput run hands the Query a complete, self-
 * contained message array: (optional) System prompt + (optional)
 * retrieved memory context + the rolling history the Session
 * already has, including the user message we just appended. Each
 * xAgentMessage borrows from session-owned storage (history entries
 * live until the run terminates or the Session is torn down) — the
 * Query still deep-copies, so once xAgentQueryRun returns this
 * transient array can go away. */

struct sess_input_view_ {
  xAgentMessage *msgs;
  xAgentContent *blocks;
  size_t         n_msgs;
  size_t         n_blocks;
  /* Heap text buffer holding the stringified ephemeral memory
   * block, referenced by one of the entries in @c blocks when
   * memory hits were injected. NULL when no hits were injected
   * or formatting failed. Freed in sess_input_view_free(). */
  char *ephemeral_text;
};

static void sess_input_view_free(struct sess_input_view_ *v) {
  free(v->msgs);
  free(v->blocks);
  free(v->ephemeral_text);
  memset(v, 0, sizeof(*v));
}

/* Render one xAgentSessionMsg pulled from the memory store into a
 * single human-readable line that we can splice into a System
 * context block. Returns the number of bytes written (excluding the
 * trailing NUL) or 0 on a kind we don't know how to render. Output
 * is truncated to @p cap bytes; caller sizes @p cap generously so
 * this rarely trims.
 */
static size_t render_memory_entry_(const xAgentSessionMsg *m, char *buf,
                                   size_t cap) {
  const char *role_str = "?";
  switch (m->role) {
  case xAgentRole_System:
    role_str = "system";
    break;
  case xAgentRole_User:
    role_str = "user";
    break;
  case xAgentRole_Assistant:
    role_str = "assistant";
    break;
  case xAgentRole_Tool:
    role_str = "tool";
    break;
  }

  int n = 0;
  switch (m->kind) {
  case xAgentSessionEntry_Text:
  case xAgentSessionEntry_Thinking:
    n = snprintf(buf, cap, "- %s: %.*s\n", role_str,
                 (int)(m->text_len > (size_t)INT_MAX ? INT_MAX : m->text_len),
                 m->text ? m->text : "");
    break;
  case xAgentSessionEntry_ToolUse:
    n = snprintf(buf, cap, "- %s (call %s: %s)\n", role_str,
                 m->tool_use_name ? m->tool_use_name : "?",
                 m->tool_use_args ? m->tool_use_args : "");
    break;
  case xAgentSessionEntry_ToolResult:
    n = snprintf(buf, cap, "- %s%s: %.*s\n", role_str,
                 m->tool_result_is_error ? " [error]" : "",
                 (int)(m->tool_result_output_len > (size_t)INT_MAX
                         ? INT_MAX
                         : m->tool_result_output_len),
                 m->tool_result_output ? m->tool_result_output : "");
    break;
  default:
    return 0;
  }
  if (n < 0) return 0;
  if ((size_t)n >= cap) return cap - 1;
  return (size_t)n;
}

/* Format an xAgentMemoryHits set into a single newline-separated
 * block of text. Caller owns the returned buffer (malloc'd). NULL
 * on OOM or when @p hits is empty. */
static char *format_memory_hits_(const xAgentMemoryHits *hits) {
  if (!hits || hits->n_entries == 0) return NULL;

  /* One pass to size the buffer, one to render. The header is
   * fixed, each entry uses a single line rendered by
   * render_memory_entry_. Upper bound: sum of all text lengths
   * plus a generous per-entry overhead for role labels and
   * formatting (64 bytes per entry is more than enough). */
  size_t cap = 64; /* header */
  for (size_t i = 0; i < hits->n_entries; i++) {
    const xAgentSessionMsg *m      = &hits->entries[i];
    size_t                  approx = 64;
    approx += m->text_len;
    approx += m->tool_result_output_len;
    if (m->tool_use_args) approx += strlen(m->tool_use_args);
    if (m->tool_use_name) approx += strlen(m->tool_use_name);
    cap += approx;
  }

  char *buf = (char *)malloc(cap);
  if (!buf) return NULL;

  size_t used = (size_t)snprintf(buf, cap, "[retrieved memory: %zu entries]\n",
                                 hits->n_entries);
  if (used >= cap) { /* shouldn't happen, but be safe */
    free(buf);
    return NULL;
  }

  for (size_t i = 0; i < hits->n_entries; i++) {
    size_t w = render_memory_entry_(&hits->entries[i], buf + used, cap - used);
    used += w;
    if (used + 1 >= cap) break;
  }
  if (used == 0) {
    free(buf);
    return NULL;
  }
  return buf;
}

/* Forward declaration — defined after session_enforce_budget_. */
static xErrno sess_input_view_build(struct xAgentSession_   *s,
                                    const xAgentMemoryHits  *hits,
                                    size_t                   hist_end,
                                    struct sess_input_view_ *out);

static xErrno session_enforce_budget_(struct xAgentSession_ *s,
                                      xAgentMessage          msg) {
  if (s->budget.policy == xAgentBudgetPolicy_Disabled) return xErrno_Ok;

  size_t limit     = session_budget_limit_(s);
  size_t incoming  = estimate_incoming_user_tokens_(msg);
  size_t current   = session_estimate_current_(s);
  size_t estimated = current + incoming;

  /* ── Step 0: Under budget — gate passes immediately ────────── */
  if (estimated <= limit) {
    /* Budget is OK — clear the anti-loop guard so a future
     * over-budget episode can try compacting from scratch. */
    s->last_compact_history_len = 0;

    /* Remember what the gate saw so sess_fwd_on_done can compare
     * it to the provider-reported prompt_tokens and update the
     * bookkeeping. Note we store the current total (history
     * including the new user turn), which is what the provider
     * not just the history side. */
    s->last_gate_total = estimated;

    /* Notify the caller that the gate passed, including the
     * token breakdown so they can display remaining capacity. */
    if (s->on_budget_event) {
      struct xAgentBudgetGateInfo gi;
      gi.limit                          = limit;
      gi.estimated                      = estimated;
      gi.remaining                      = limit - estimated;
      gi.last_first_round_prompt_tokens = s->last_first_round_prompt_tokens;
      s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_GatePassed, &gi,
                         s->budget_event_ud);
    }

    return xErrno_Ok;
  }

  /* ── Step 1: Trim consumed tool results (synchronous) ───────
   *
   * Before falling into the heavy-weight compact path, try a
   * lighter approach: shrink consumed tool_result outputs in-place.
   * This frees token budget without removing entire turns,
   * preserving conversation flow. */
  {
    ssize_t bytes_freed = 0;
    size_t  trimmed     = session_trim_consumed_tool_results_(s, &bytes_freed);
    (void)trimmed;
    (void)bytes_freed;

    /* Re-estimate after trimming. */
    current   = session_estimate_current_(s);
    estimated = current + incoming;

    if (estimated <= limit) {
      /* Trimming brought us under budget. */
      s->last_compact_history_len = 0;
      s->last_gate_total          = estimated;

      if (s->on_budget_event) {
        struct xAgentBudgetGateInfo gi;
        gi.limit                          = limit;
        gi.estimated                      = estimated;
        gi.remaining                      = limit - estimated;
        gi.last_first_round_prompt_tokens = s->last_first_round_prompt_tokens;
        s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_GatePassed, &gi,
                           s->budget_event_ud);
      }

      return xErrno_Ok;
    }
  }

  /* ── Step 2: Compact (asynchronous) ─────────────────────────
   *
   * Launch an internal summary Query that compresses old history
   * into one System summary entry. The compact runs asynchronously;
   * session_enforce_budget_ returns xErrno_Busy so the caller
   * knows to wait. sess_fwd_on_done handles the compact completion
   * and auto-retries the pending input.
   *
   * The compact replaces history[compact_start..compact_end) with
   * one summary entry. compact_start is the index of the user turn
   * at position context_preserve_head_turns; compact_end is the
   * index of the user turn at position
   * (user_count - context_preserve_tail_turns). The head and tail
   * turns are preserved; everything in between is compacted. */
  switch (s->budget.policy) {
  case xAgentBudgetPolicy_Error:
    return xErrno_PromptTooLong;

  case xAgentBudgetPolicy_Summarize: {
    size_t                          hlen = xArrayLen(s->history_arr);
    const struct xAgentSessionMsg_ *msgs_view =
      (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr);

    /* Count user turns in history. */
    size_t user_count = 0;
    for (size_t i = 0; i < hlen; ++i) {
      if (msgs_view[i].role == xAgentRole_User) ++user_count;
    }

    /* If no user turns, there's nothing meaningful to compact. */
    if (user_count == 0) return xErrno_PromptTooLong;

    /* Determine how many head/tail turns to preserve.
     * 0 = no turns preserved on that side. */
    size_t keep_head = s->budget.context_preserve_head_turns;
    size_t keep_tail = s->budget.context_preserve_tail_turns;

    /* Not enough turns to compact. */
    if (keep_head + keep_tail >= user_count) return xErrno_PromptTooLong;

    /* compact_start = index of the first user turn to compact
     * (= user turn at position keep_head).
     * compact_end = hlen when keep_tail == 0 (compact to the end),
     * otherwise the index of the first preserved tail user turn. */
    size_t compact_start =
      ai_budget_find_user_turn(msgs_view, hlen, keep_head);
    size_t compact_end;
    if (keep_tail == 0) {
      compact_end = hlen;
    } else {
      compact_end =
        ai_budget_find_user_turn(msgs_view, hlen, user_count - keep_tail);
    }
    if (compact_start == XAGENT_BUDGET_NO_SUCH_TURN ||
        compact_end == XAGENT_BUDGET_NO_SUCH_TURN ||
        compact_start >= compact_end) {
      return xErrno_PromptTooLong;
    }


    /* Anti-loop guard: if last_compact_history_len > 0 and
     * hlen <= last_compact_history_len, another compact won't
     * help. This guards against estimation inaccuracies that
     * could cause an infinite compact loop. */
    if (s->last_compact_history_len > 0 &&
        hlen <= s->last_compact_history_len) {
      return xErrno_PromptTooLong;
    }

    /* Re-entrance guard: only one compact at a time. */
    if (s->compacting) return xErrno_Busy;

    /* Save the incoming user message text for auto-retry after
     * the compact completes. Concatenate all text blocks into
     * one string, matching history_append_user_msg's behaviour. */
    {
      size_t total_len = 0;
      for (size_t i = 0; i < msg.n; i++) {
        if (msg.contents[i].type == xAgentContentType_Text) {
          total_len += msg.contents[i].u.text.len;
        }
      }

      if (total_len > 0) {
        char *buf = (char *)malloc(total_len + 1);
        if (buf) {
          size_t off = 0;
          for (size_t i = 0; i < msg.n; i++) {
            if (msg.contents[i].type == xAgentContentType_Text) {
              size_t n = msg.contents[i].u.text.len;
              if (n) memcpy(buf + off, msg.contents[i].u.text.text, n);
              off += n;
            }
          }
          buf[total_len] = '\0';
          free(s->pending_text);
          s->pending_text     = buf;
          s->pending_text_len = total_len;
        }
      }
    }

    /* Record compact boundary and anti-loop state. */
    s->compact_start_idx        = compact_start;
    s->compact_end_idx          = compact_end;
    s->last_compact_history_len = hlen;

    /* Notify caller: compact is starting. */
    if (s->on_budget_event) {
      struct xAgentBudgetCompactInfo ci;
      ci.entries_compacted = compact_end;
      s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_Compacting, &ci,
                         s->budget_event_ud);
    }

    /* Build the conversation view for history[0..compact_end)
     * using sess_input_view_build so the message array is
     * byte-identical to what a normal request would produce —
     * maximising prompt cache hit rate. The summary instruction
     * tells the model to summarize from the compact_start turn.
     * We then append a summary instruction as the final User
     * message. */
    struct sess_input_view_ hist_view;
    xErrno                  vrc =
      sess_input_view_build(s, /*hits=*/NULL, compact_end, &hist_view);
    if (vrc != xErrno_Ok) return xErrno_PromptTooLong;

    const size_t keep = compact_end - compact_start;
    char         summary_instr[512];
    snprintf(summary_instr, sizeof(summary_instr),
             XAGENT_SUMMARY_INSTRUCT_PROMPT, keep);

    /* Extend the view by one message (the summary instruction).
     * CAUTION: we must NOT realloc hist_view.blocks because every
     * existing msgs[i].contents pointer points into that array.
     * Instead, allocate the extra block separately and use
     * malloc+memcpy for the msgs array so the old contents
     * pointers remain valid. */
    size_t         n_msgs = hist_view.n_msgs + 1;
    xAgentMessage *msgs =
      (xAgentMessage *)malloc(n_msgs * sizeof(xAgentMessage));
    xAgentContent *sum_blk = (xAgentContent *)calloc(1, sizeof(xAgentContent));
    if (!msgs || !sum_blk) {
      free(msgs);
      free(sum_blk);
      free(hist_view.ephemeral_text);
      free(hist_view.blocks);
      free(hist_view.msgs);
      return xErrno_PromptTooLong;
    }
    memcpy(msgs, hist_view.msgs, hist_view.n_msgs * sizeof(xAgentMessage));
    memset(&msgs[hist_view.n_msgs], 0, sizeof(xAgentMessage));

    /* Append the summary instruction user message. */
    sum_blk->type                   = xAgentContentType_Text;
    sum_blk->u.text.text            = summary_instr;
    sum_blk->u.text.len             = strlen(summary_instr);
    msgs[hist_view.n_msgs].role     = xAgentRole_User;
    msgs[hist_view.n_msgs].contents = sum_blk;
    msgs[hist_view.n_msgs].n        = 1;

    /* Create an internal Query for the summary task.
     * - The Query's only callback is on_done so we can harvest
     *   the summary text.
     * - Budget enforcement is implicitly disabled: the compact
     *   Query is created by session_enforce_budget_ which gates
     *   on s->compacting, so no recursive budget check occurs.
     * - max_tokens is hardcoded to 1024 (not from conf). */
    struct xAgent_ *a  = (struct xAgent_ *)s->agent;
    xAgentQueryConf qc = {0};
    qc.cbs.on_done     = sess_fwd_on_done;
    qc.cbs.user_data   = s;
    qc.provider        = a->provider;
    qc.tools           = (const xAgentTool **)a->tools;
    qc.tools_count     = a->tools_count;
    qc.model           = s->model;
    qc.max_tokens      = 1024;
    qc.max_turns       = 1; /* single round: no tool loop */
    qc.session         = (xAgentSession)s;

    xAgentQuery q = xAgentQueryCreate(&qc);
    if (!q) {
      free(msgs);
      free(sum_blk);
      free(hist_view.ephemeral_text);
      free(hist_view.blocks);
      free(hist_view.msgs);
      return xErrno_NoMemory;
    }

    /* Mark compacting before Run so the Session rejects new
     * inputs during the compact. */
    s->compacting = 1;

    xErrno rc = xAgentQueryRun(q, msgs, n_msgs);
    /* xAgentQueryRun deep-copies everything, so we can release
     * the combined array immediately. */
    free(msgs);
    free(sum_blk);
    free(hist_view.ephemeral_text);
    free(hist_view.blocks);
    free(hist_view.msgs);

    if (rc != xErrno_Ok) {
      /* Compact query failed to start — clean up and refuse.
       * History is untouched so the caller can relax the budget
       * or surface an error. */
      s->compacting = 0;
      xAgentQueryDestroy(q);
      return xErrno_PromptTooLong;
    }

    /* Compact query is now in flight. Return Busy so the caller
     * knows to wait. sess_fwd_on_done will handle the compact
     * completion and auto-retry the pending input. */
    return xErrno_Busy;
  }

  case xAgentBudgetPolicy_Disabled:
  default:
    /* Reserved / unknown policies fall through to a hard refuse. */
    return xErrno_PromptTooLong;
  }
}

/* Build a message array from the current session state. Consecutive
 * Assistant entries are folded into one xAgentMessage (so thinking +
 * text + tool_use blocks travel together); other roles map 1:1.
 *
 * When @p hits is non-NULL and has entries, a single extra System
 * message is spliced in immediately after the session's system
 * prompt (if any), carrying a rendered summary of the memory hits.
 * The block is ephemeral: it never touches history_arr and lives
 * only for this one run (freed in sess_input_view_free). */
static xErrno sess_input_view_build(struct xAgentSession_   *s,
                                    const xAgentMemoryHits  *hits,
                                    size_t                   hist_end,
                                    struct sess_input_view_ *out) {
  memset(out, 0, sizeof(*out));
  size_t extra_system = (s->system_prompt && s->system_prompt[0]) ? 1 : 0;

  /* Lazily render the memory block. We do it up front (before
   * Pass 1) so we know whether to budget a slot for it. */
  char *mem_text = NULL;
  if (hits && hits->n_entries > 0) {
    mem_text = format_memory_hits_(hits);
  }
  size_t extra_mem = mem_text ? 1 : 0;

  /* hist_end == 0 means "use the full history". */
  size_t hist_len = hist_end ? hist_end : xArrayLen(s->history_arr);

  /* Pass 1: count. */
  size_t n_msgs   = extra_system + extra_mem;
  size_t n_blocks = extra_system + extra_mem;
  for (size_t i = 0; i < hist_len;) {
    struct xAgentSessionMsg_ *m =
      (struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, i);
    if (m->role == xAgentRole_Assistant) {
      size_t j = i;
      while (j < hist_len &&
             ((struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, j))->role ==
               xAgentRole_Assistant)
        j++;
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

  out->msgs   = (xAgentMessage *)calloc(n_msgs, sizeof(xAgentMessage));
  out->blocks = (xAgentContent *)calloc(n_blocks, sizeof(xAgentContent));
  if (!out->msgs || !out->blocks) {
    free(mem_text);
    sess_input_view_free(out);
    return xErrno_NoMemory;
  }
  out->n_msgs         = n_msgs;
  out->n_blocks       = n_blocks;
  out->ephemeral_text = mem_text; /* transferred; freed in view_free */

  /* Pass 2: populate. */
  size_t mi = 0;
  size_t bi = 0;

  if (extra_system) {
    out->blocks[bi].type        = xAgentContentType_Text;
    out->blocks[bi].u.text.text = s->system_prompt;
    out->blocks[bi].u.text.len  = strlen(s->system_prompt);
    out->msgs[mi].role          = xAgentRole_System;
    out->msgs[mi].contents      = &out->blocks[bi];
    out->msgs[mi].n             = 1;
    mi++;
    bi++;
  }

  if (extra_mem) {
    /* One-shot System message carrying the memory hits as plain
     * text. It rides alongside any existing system prompt rather
     * than being folded into it so the model can tell the two
     * apart and so a future provider-level "context" channel can
     * intercept this specific block without re-parsing the base
     * prompt. */
    out->blocks[bi].type        = xAgentContentType_Text;
    out->blocks[bi].u.text.text = out->ephemeral_text;
    out->blocks[bi].u.text.len  = strlen(out->ephemeral_text);
    out->msgs[mi].role          = xAgentRole_System;
    out->msgs[mi].contents      = &out->blocks[bi];
    out->msgs[mi].n             = 1;
    mi++;
    bi++;
  }

  for (size_t i = 0; i < hist_len;) {
    struct xAgentSessionMsg_ *m =
      (struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, i);
    if (m->role == xAgentRole_Assistant) {
      size_t block_start = bi;
      size_t j           = i;
      while (j < hist_len) {
        struct xAgentSessionMsg_ *mm =
          (struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, j);
        if (mm->role != xAgentRole_Assistant) break;
        xAgentContent *b = &out->blocks[bi++];
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

  return xErrno_Ok;
}

/* ── Query → Session callback forwarding ──────────────────────────────
 *
 * The Query fires its own @c q->cbs.* stream with a Query handle.
 * Sessions install the shims below so every event is re-dispatched
 * to the caller's xAgentSessionCallbacks with the Session handle and
 * the caller's user_data. The Query callbacks carry the owning
 * Session as user_data.
 */

static void sess_fwd_on_text(xAgentQuery q, const char *chunk, size_t len,
                             void *ud) {
  (void)q;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;
  if (s->cbs.on_text) {
    s->cbs.on_text((xAgentSession)s, chunk, len, s->cbs.user_data);
  }
}

static void sess_fwd_on_thinking(xAgentQuery q, const char *chunk, size_t len,
                                 void *ud) {
  (void)q;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;
  if (s->cbs.on_thinking) {
    s->cbs.on_thinking((xAgentSession)s, chunk, len, s->cbs.user_data);
  }
}

static void sess_fwd_on_error(xAgentQuery q, xErrno err, const char *msg,
                              void *ud) {
  (void)q;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;
  if (s->cbs.on_error) {
    s->cbs.on_error((xAgentSession)s, err, msg, s->cbs.user_data);
  }
}

static void sess_fwd_on_tool(xAgentQuery q, const char *tool_name, int started,
                             void *ud) {
  (void)q;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;
  if (s->cbs.on_tool) {
    s->cbs.on_tool((xAgentSession)s, tool_name, started, s->cbs.user_data);
  }
}

/* ── Sidecar chunk accumulator ────────────────────────────────────
 * Per-chunk entry stored in the sidecar_output xArray so the
 * sidecar Query has the accumulated tool output when it fires. */
struct sidecar_chunk_ {
  char  *data;
  size_t len;
};

static void sidecar_chunk_release(void *elem) {
  struct sidecar_chunk_ *c = (struct sidecar_chunk_ *)elem;
  free(c->data);
  c->data = NULL;
}

static const xArrayCallbacks kSidecarChunkCbs = {
  .release = sidecar_chunk_release,
};

static void sess_fwd_on_tool_output(xAgentQuery q, const char *tool_use_id,
                                    const char *tool_name, const char *data,
                                    size_t len, void *ud) {
  (void)q;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;
  if (s->cbs.on_tool_output) {
    s->cbs.on_tool_output((xAgentSession)s, tool_use_id, tool_name, data, len,
                          s->cbs.user_data);
  }

  /* ── Sidecar idle-detection ──────────────────────────────────
   * Track the last time an async tool produced output. If the
   * session has sidecar_idle_ms configured, (re-)arm a timer so
   * that a sidecar Query is launched when the tool goes idle. */
  struct xAgent_ *a = (struct xAgent_ *)s->agent;
  if (s->sidecar_idle_ms > 0 && a->enable_sidecar_query && s->query &&
      !s->sidecar) {

    /* Record which tool_use_id is producing output (the one most
     * likely to need stdin if it stalls). */
    if (!s->sidecar_tool_use_id && tool_use_id) {
      s->sidecar_tool_use_id = dup_bytes(tool_use_id, strlen(tool_use_id));
    }

    /* Accumulate the output chunk so the sidecar has context
     * when it fires. Lazy-create the accumulator on first chunk. */
    if (!s->sidecar_output) {
      s->sidecar_output =
        xArrayCreate(sizeof(struct sidecar_chunk_), 16, &kSidecarChunkCbs);
    }
    if (s->sidecar_output) {
      struct sidecar_chunk_ *slot =
        (struct sidecar_chunk_ *)xArrayPush(&s->sidecar_output);
      if (slot) {
        slot->data = dup_bytes(data, len);
        slot->len  = len;
      }
    }

    s->sidecar_last_output_ms = xMonoMs();

    /* Cancel any pending idle timer before scheduling a new one. */
    if (s->sidecar_idle_timer) {
      xEventLoopTimerCancel(a->loop, s->sidecar_idle_timer);
      s->sidecar_idle_timer = NULL;
    }

    s->sidecar_idle_timer = xEventLoopTimerAfter(
      a->loop, session_sidecar_idle_timer_cb, s, s->sidecar_idle_ms);
  }
}

/* Forward the query-level confirmation gate up to the session-level
 * host callback. The resolver handle is the same concrete type on
 * both layers \u2014 we just translate the xAgentQuery signature to the
 * xAgentSession one and pass the host\u2019s user_data. If the host did
 * not wire on_tool_confirm, this forwarder is never stamped into
 * the query\u2019s callback table, so we cannot get called at all. */
static void sess_fwd_on_tool_confirm(xAgentQuery q, const char *tool_name,
                                     const char               *tool_use_id,
                                     const char               *args_json,
                                     xAgentToolConfirmResolver resolver,
                                     void                     *ud) {
  (void)q;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;
  if (s->cbs.on_tool_confirm) {
    s->cbs.on_tool_confirm((xAgentSession)s, tool_name, tool_use_id, args_json,
                           resolver, s->cbs.user_data);
    return;
  }
  /* Defensive fallback: the callback table should not be stamped
   * with sess_fwd_on_tool_confirm when the host\u2019s handler is NULL,
   * but if we still arrived here just auto-allow to preserve the
   * legacy behaviour of "needs_confirm without a host = no gate". */
  xAgentToolConfirmResolve(resolver, xAgentToolDecision_Allow, NULL);
}

/* ── Sidecar Query implementation ──────────────────────────────────
 *
 * A sidecar Query is a lightweight, Session-managed Query that runs
 * alongside the main Query when an async tool call has gone idle
 * (no streaming output for sidecar_idle_ms). The sidecar is given
 * the session's rolling conversation history, the accumulated tool
 * output, and a restricted tool set so the AI can decide whether
 * to send input, cancel, or take other action.
 *
 * Message layout sent to the sidecar:
 *   1. Sidecar system prompt (diagnostic-assistant instructions).
 *   2. Session's rolling history (user requests, assistant replies,
 *      tool calls/results — everything the main Query has produced
 *      so far). This gives the sidecar the context it needs to
 *      understand *why* the tool was invoked and what input might
 *      be appropriate.
 *   3. Idle-tool user message with the accumulated tool output.
 *
 * Lifecycle:
 *   1. Timer fires → session_sidecar_idle_timer_cb
 *   2. Create Query with restricted tools, run it
 *   3. Query completes → session_sidecar_on_done → cleanup
 *
 * The sidecar does NOT replace the main Query and does NOT occupy
 * s->query. At most one sidecar is alive at a time. */

/* System prompt for the sidecar Query — instructs the AI to analyse
 * the situation and decide whether to interact with the idle tool.
 * The sidecar receives the full conversation history so it can
 * understand the user's original intent and what the assistant was
 * trying to accomplish. */
#define XAGENT_SIDECAR_SYSTEM_PROMPT                                  \
  "You are a diagnostic assistant. A tool call is blocking the main " \
  "conversation because it has not produced output for a while. "     \
  "You are given the conversation history above for context so you "  \
  "can understand what the user asked and what the assistant was "    \
  "doing. Analyse the tool output shown below and decide whether to " \
  "send input to the running command. If the command is waiting for " \
  "user input (e.g. a prompt, confirmation, or REPL), use the "       \
  "shell_stdin tool to send the appropriate input. If the command "   \
  "appears to be genuinely running (e.g. compiling, downloading), "   \
  "respond with empty text — no action is needed."

static void session_sidecar_cleanup(struct xAgentSession_ *s) {
  struct xAgent_ *a = (struct xAgent_ *)s->agent;

  /* Cancel any pending idle timer. */
  if (s->sidecar_idle_timer) {
    xEventLoopTimerCancel(a->loop, s->sidecar_idle_timer);
    s->sidecar_idle_timer = NULL;
  }

  /* Destroy the sidecar Query if still alive. */
  if (s->sidecar) {
    xAgentQueryDestroy((xAgentQuery)s->sidecar);
    s->sidecar = NULL;
  }

  /* Free the tool_use_id copy. */
  free(s->sidecar_tool_use_id);
  s->sidecar_tool_use_id = NULL;

  /* Destroy the output accumulator. */
  xArrayDestroy(s->sidecar_output);
  s->sidecar_output = NULL;

  s->sidecar_last_output_ms = 0;
}

/* Called when the sidecar Query completes (any reason). */
static void session_sidecar_on_done(xAgentQuery q, xAgentDoneReason reason,
                                    const xAgentUsage *usage, void *ud) {
  (void)reason;
  (void)usage;
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;

  /* If the main Query is still running and this sidecar produced
   * text output, forward it to the caller via on_tool_output so
   * they can see what the sidecar decided to do. */
  if (s->query && q) {
    struct xAgentSessionMsg_ *produced   = NULL;
    size_t                    n_produced = 0;
    ai_query_take_produced((struct xAgentQuery_ *)q, &produced, &n_produced);

    for (size_t i = 0; i < n_produced; i++) {
      if (produced[i].kind == xAgentSessionEntry_Text && produced[i].text &&
          produced[i].text_len > 0) {
        /* Sidecar text is informational — just forward it. */
        if (s->cbs.on_text) {
          s->cbs.on_text((xAgentSession)s, produced[i].text,
                         produced[i].text_len, s->cbs.user_data);
        }
      }
    }
  }

  /* Notify the caller that the sidecar query has completed. */
  if (s->cbs.on_sidecar) {
    s->cbs.on_sidecar((xAgentSession)s, xAgentSidecarEvent_Done,
                      s->cbs.user_data);
  }

  /* Clean up the sidecar state. */
  session_sidecar_cleanup(s);
}

/* Timer callback: the main Query's async tool has been idle for
 * sidecar_idle_ms. Launch a sidecar Query so the AI can inspect
 * the situation. */
static void session_sidecar_idle_timer_cb(void *arg) {
  struct xAgentSession_ *s = (struct xAgentSession_ *)arg;
  s->sidecar_idle_timer    = NULL; /* timer has fired, handle is stale */

  /* Guard: only launch if the main Query is still in flight, the
   * session is not compacting, and no sidecar is already running. */
  if (!s->query || s->compacting || s->sidecar) return;

  /* Guard: check that the async tool is actually still pending
   * (it may have completed between the timer being scheduled and
   * now). */
  struct xAgentQuery_ *q = (struct xAgentQuery_ *)s->query;
  if (xArrayLen(q->async_pending_arr) == 0) return;

  struct xAgent_ *a = (struct xAgent_ *)s->agent;

  /* ── Build the sidecar's input messages ───────────────────────
   *   1. System prompt (sidecar-specific, replaces session's).
   *   2. Session's rolling conversation history so the sidecar has
   *      context about what the user originally asked and what the
   *      assistant has been doing.
   *   3. User message describing the idle tool and its output. */

  /* Concatenate the accumulated output chunks into one string. */
  size_t total_len = 0;
  size_t n_chunks  = xArrayLen(s->sidecar_output);
  for (size_t i = 0; i < n_chunks; i++) {
    struct sidecar_chunk_ *c =
      (struct sidecar_chunk_ *)xArrayAt(s->sidecar_output, i);
    total_len += c->len;
  }

  char *output_text = NULL;
  if (total_len > 0) {
    output_text = (char *)malloc(total_len + 1);
    if (output_text) {
      size_t ooff = 0;
      for (size_t i = 0; i < n_chunks; i++) {
        struct sidecar_chunk_ *c =
          (struct sidecar_chunk_ *)xArrayAt(s->sidecar_output, i);
        if (c->data && c->len > 0) {
          memcpy(output_text + ooff, c->data, c->len);
          ooff += c->len;
        }
      }
      output_text[ooff] = '\0';
    }
  }

  /* Build the user message content: describe the idle tool. */
  static const char kIdlePrefix[] =
    "[idle tool] The following async tool call has produced no output "
    "for a while. tool_use_id=\"";
  static const char kIdleMid[] = "\"\n\nLast accumulated output:\n";

  const char *tid =
    s->sidecar_tool_use_id ? s->sidecar_tool_use_id : "(unknown)";
  size_t tid_len    = strlen(tid);
  size_t prefix_len = sizeof(kIdlePrefix) - 1;
  size_t mid_len    = sizeof(kIdleMid) - 1;
  size_t user_len   = prefix_len + tid_len + mid_len + total_len;

  char *user_text = (char *)malloc(user_len + 1);
  if (!user_text) {
    free(output_text);
    return;
  }
  size_t uoff = 0;
  memcpy(user_text + uoff, kIdlePrefix, prefix_len);
  uoff += prefix_len;
  memcpy(user_text + uoff, tid, tid_len);
  uoff += tid_len;
  memcpy(user_text + uoff, kIdleMid, mid_len);
  uoff += mid_len;
  if (output_text && total_len > 0) {
    memcpy(user_text + uoff, output_text, total_len);
    uoff += total_len;
  }
  user_text[uoff] = '\0';
  free(output_text);

  /* Build the session's conversation history view so the sidecar
   * has context about the user's original request and the assistant's
   * actions so far. Sidecar deliberately passes hits=NULL — it runs
   * its own narrow analysis and does not want main-run memory
   * context muddying its prompt. */
  struct sess_input_view_ hist_view;
  xErrno                  vrc =
    sess_input_view_build(s, /*hits=*/NULL, /*hist_end=*/0, &hist_view);
  if (vrc != xErrno_Ok) {
    free(user_text);
    return;
  }

  /* Determine whether the history view starts with a system prompt
   * (we'll skip it and use the sidecar-specific one instead). */
  int has_system =
    (hist_view.n_msgs > 0 && hist_view.msgs[0].role == xAgentRole_System);
  size_t hist_skip = has_system ? 1 : 0;

  /* ── Find the shell_stdin tool among the agent's tools ──────── */
  xAgentTool stdin_tool = NULL;
  for (size_t i = 0; i < a->tools_count; i++) {
    if (a->tools[i]) {
      const char *name = ai_tool_name((xAgentTool)a->tools[i]);
      if (name && strcmp(name, "shell_stdin") == 0) {
        stdin_tool = (xAgentTool)a->tools[i];
        break;
      }
    }
  }

  /* Build the tools array for the sidecar: only shell_stdin (if
   * available). If shell_stdin is not registered, the sidecar
   * runs without tools — it can only produce text advice. */
  const xAgentTool *sidecar_tools[1];
  size_t            sidecar_tools_count = 0;
  if (stdin_tool) {
    sidecar_tools[0]    = stdin_tool;
    sidecar_tools_count = 1;
  }

  /* ── Allocate the combined message array ───────────────────────
   * Layout: [sidecar_system] + [history_msgs (minus original system)]
   *       + [idle_user]
   * We always emit our own sidecar system prompt; if the history
   * view contained one it is skipped (hist_skip == 1). */
  size_t n_hist = hist_view.n_msgs - hist_skip;
  /* +1 for the sidecar system msg + +1 for the idle user message. */
  size_t n_total_msgs = 1 + n_hist + 1;

  xAgentMessage *msgs =
    (xAgentMessage *)calloc(n_total_msgs, sizeof(xAgentMessage));
  xAgentContent *extra_blocks =
    (xAgentContent *)calloc(2, sizeof(xAgentContent));
  if (!msgs || !extra_blocks) {
    free(msgs);
    free(extra_blocks);
    free(user_text);
    sess_input_view_free(&hist_view);
    return;
  }

  size_t mi = 0;

  /* Sidecar system prompt — always the first message. */
  extra_blocks[0].type        = xAgentContentType_Text;
  extra_blocks[0].u.text.text = XAGENT_SIDECAR_SYSTEM_PROMPT;
  extra_blocks[0].u.text.len  = strlen(XAGENT_SIDECAR_SYSTEM_PROMPT);
  msgs[mi].role               = xAgentRole_System;
  msgs[mi].contents           = &extra_blocks[0];
  msgs[mi].n                  = 1;
  mi++;

  /* Copy the history view messages (skipping the original system
   * prompt if present). */
  if (n_hist > 0) {
    memcpy(&msgs[mi], &hist_view.msgs[hist_skip],
           n_hist * sizeof(xAgentMessage));
    mi += n_hist;
  }
  /* Append the idle user message. */
  extra_blocks[1].type        = xAgentContentType_Text;
  extra_blocks[1].u.text.text = user_text;
  extra_blocks[1].u.text.len  = uoff;
  msgs[mi].role               = xAgentRole_User;
  msgs[mi].contents           = &extra_blocks[1];
  msgs[mi].n                  = 1;
  mi++;

  /* ── Create and run the sidecar Query ──────────────────────── */
  xAgentQueryConf qc = {0};
  qc.cbs.on_done     = session_sidecar_on_done;
  qc.cbs.user_data   = s;
  /* Honour any per-session provider/model override; fall back to
   * the agent's defaults when none is set. See
   * xAgentSessionSetProvider() / xAgentSessionSetModel(). */
  qc.provider    = s->provider_override ? s->provider_override : a->provider;
  qc.tools       = sidecar_tools_count > 0 ? sidecar_tools : NULL;
  qc.tools_count = sidecar_tools_count;
  qc.model       = s->model;
  qc.max_tokens  = 256; /* sidecar should be concise */
  qc.max_turns   = 1;   /* single round: analyse + act */
  qc.session     = (xAgentSession)s;

  xAgentQuery sq = xAgentQueryCreate(&qc);
  if (!sq) {
    free(msgs);
    free(extra_blocks);
    free(user_text);
    sess_input_view_free(&hist_view);
    return;
  }

  xErrno rc = xAgentQueryRun(sq, msgs, mi);

  /* xAgentQueryRun deep-copies everything, so we can release the
   * combined array and the history view immediately. */
  free(msgs);
  free(extra_blocks);
  free(user_text);
  sess_input_view_free(&hist_view);

  if (rc != xErrno_Ok) {
    xAgentQueryDestroy(sq);
    return;
  }

  s->sidecar = (struct xAgentQuery_ *)sq;

  /* Notify the caller that a sidecar query has started. */
  if (s->cbs.on_sidecar) {
    s->cbs.on_sidecar((xAgentSession)s, xAgentSidecarEvent_Started,
                      s->cbs.user_data);
  }
}

/* ── Auto-retry pending input after compact ───────────────────────
 *
 * Called from sess_fwd_on_done after a compact completes
 * successfully. If a pending user message was saved before the
 * compact was launched, re-submit it via xAgentSessionInput.
 * The budget gate will re-evaluate (hopefully the compact freed
 * enough space); if still over budget the gate may trim more tool
 * results or refuse with PromptTooLong. In the rare case where
 * auto-retry fails, the pending message is simply lost — the caller
 * will see no response and can retry manually. */
static void session_auto_retry_pending_(struct xAgentSession_ *s) {
  if (!s->pending_text) return;
  char *text          = s->pending_text;
  s->pending_text     = NULL;
  size_t len          = s->pending_text_len;
  s->pending_text_len = 0;

  /* Construct xAgentMessage from the saved text. */
  xAgentContent content;
  memset(&content, 0, sizeof(content));
  content.type        = xAgentContentType_Text;
  content.u.text.text = text;
  content.u.text.len  = len;

  xAgentMessage msg;
  memset(&msg, 0, sizeof(msg));
  msg.role     = xAgentRole_User;
  msg.contents = &content;
  msg.n        = 1;

  /* xAgentSessionInput deep-copies the message, so it is safe
   * to free text after the call returns. */
  xAgentSessionInput((xAgentSession)s, msg);
  free(text);
}

/* Terminal forwarding: pull the Query's produced-turn list into the
 * Session's history first (so the on_done handler sees the updated
 * conversation), then fire the caller's on_done, then release the
 * Query. Destroy-in-on_done is safe because query.c's finalize path
 * does not touch @p q after firing our callback.
 *
 * Order note: caller's on_done runs while @c s->query still points
 * at @p q, so xAgentSessionQuery / xAgentQueryUsage observed from inside
 * on_done still work. Only after the callback returns do we detach
 * and free (xAgentQueryDestroy nulls s->query for us).
 *
 * Produced merging happens on every terminal reason, including
 * Aborted / MaxTurns / errors: whatever partial output the model
 * managed to emit before the run ended is legitimate conversation
 * history and the next xAgentSessionInput should see it. */
static void sess_fwd_on_done(xAgentQuery q, xAgentDoneReason reason,
                             const xAgentUsage *usage, void *ud) {
  struct xAgentSession_ *s = (struct xAgentSession_ *)ud;

  /* Steal the produced list (ownership transfer). After this the
   * Query's own destructor won't touch these entries. */
  struct xAgentSessionMsg_ *produced   = NULL;
  size_t                    n_produced = 0;
  ai_query_take_produced((struct xAgentQuery_ *)q, &produced, &n_produced);

  /* ── Summarize compact completion ─────────────────────────
   *
   * When a compact (summary) Query completes, we need to:
   *   1. Extract the summary text from the produced entries.
   *   2. Replace the old history entries [0, compact_end_idx)
   *      with one System summary entry.
   *   3. Auto-retry the pending user message.
   *
   * The compact on_done does NOT fire the caller's on_done —
   * this is an internal operation. The caller is still waiting
   * (they received xErrno_Busy from xAgentSessionInput). */
  if (s->compacting) {
    /* Extract summary text: concatenate all text entries from
     * produced. "Thinking" models (DeepSeek-R1, Claude with
     * extended thinking) may emit a Thinking entry but no Text
     * entry when the output token budget is exhausted by the
     * reasoning phase.  Fall back to the Thinking content in
     * that case so the compact still produces a usable summary
     * rather than degrading to truncate every time. */
    size_t summary_bytes = 0;
    int    text_found    = 0;
    for (size_t i = 0; i < n_produced; i++) {
      if (produced[i].kind == xAgentSessionEntry_Text && produced[i].text) {
        summary_bytes += produced[i].text_len;
        text_found = 1;
      }
    }
    /* Fallback: no Text entries but Thinking entries exist. */
    enum xAgentSessionEntryKind_ summary_kind = xAgentSessionEntry_Text;
    if (!text_found) {
      for (size_t i = 0; i < n_produced; i++) {
        if (produced[i].kind == xAgentSessionEntry_Thinking &&
            produced[i].text) {
          summary_bytes += produced[i].text_len;
        }
      }
      if (summary_bytes > 0) summary_kind = xAgentSessionEntry_Thinking;
    }

    /* Build the summary string with a "[summary]" prefix so
     * downstream consumers can identify it. */
    static const char kSummaryPrefix[] = "[summary] ";
    size_t            prefix_len       = strlen(kSummaryPrefix);
    char             *summary_text     = NULL;

    if (summary_bytes > 0) {
      summary_text = (char *)calloc(prefix_len + summary_bytes + 1, 1);
      if (summary_text) {
        memcpy(summary_text, kSummaryPrefix, prefix_len);
        size_t off = prefix_len;
        for (size_t i = 0; i < n_produced; i++) {
          if (produced[i].kind == summary_kind && produced[i].text) {
            memcpy(summary_text + off, produced[i].text, produced[i].text_len);
            off += produced[i].text_len;
          }
        }
      }
    }

    /* Compact succeeded (we got a non-empty summary) — splice the
     * summary into history, replacing entries
     * [compact_start, compact_end). Compact failed (empty / OOM) —
     * leave history untouched. */
    size_t compact_start = s->compact_start_idx;
    size_t compact_end   = s->compact_end_idx;
    int    compact_ok    = (summary_text != NULL && summary_bytes > 0);

    if (compact_ok) {
      /* L1 preserve: deliver the about-to-be-replaced entries
       * [compact_start, compact_end) before they are swapped out
       * by the summary. Entries already in the external store
       * (persisted_prefix) are skipped so compaction doesn't
       * re-append them. */
      if (s->on_l1_preserve && compact_end > compact_start) {
        size_t skip_start = compact_start;
        if (s->persisted_prefix > compact_start) {
          skip_start = s->persisted_prefix < compact_end
                         ? s->persisted_prefix
                         : compact_end;
        }
        if (compact_end > skip_start) {
          s->on_l1_preserve(
            (xAgentSession)s,
            (const xAgentSessionMsg *)xArrayData(s->history_arr) + skip_start,
            compact_end - skip_start, xAgentL1PreserveReason_Compacted,
            s->l1_preserve_owner);
        }
      }

      /* Remove entries [compact_start, compact_end). */
      size_t remove_count = compact_end - compact_start;
      xArrayRemoveRange(s->history_arr, compact_start, remove_count);
      /* Keep persisted_prefix consistent. If the compacted range
       * overlaps with the persisted prefix, adjust it. */
      if (s->persisted_prefix > compact_start) {
        size_t overlap_end = s->persisted_prefix < compact_end
                               ? s->persisted_prefix
                               : compact_end;
        s->persisted_prefix -= (overlap_end - compact_start);
      }

      /* Build the summary entry and insert at compact_start. */
      struct xAgentSessionMsg_ summary_entry;
      memset(&summary_entry, 0, sizeof(summary_entry));
      summary_entry.role          = xAgentRole_Assistant;
      summary_entry.kind          = xAgentSessionEntry_Text;
      summary_entry.text          = summary_text;
      summary_entry.text_len      = prefix_len + summary_bytes;
      summary_entry.created_at_ms = xWallMs();
      summary_entry.is_summary    = 1;

      if (xArrayInsert(&s->history_arr, compact_start, &summary_entry)
          != xErrno_Ok) {
        /* OOM on insert — free the summary text and degrade. */
        free(summary_text);
        compact_ok = 0;
      } else {
        /* summary_text is now owned by the history array (it will
         * be freed by session_msg_release on removal). Clear the
         * local pointer so we don't double-free below. */
        summary_text = NULL;

        /* L1-preserve the new summary entry so it is written to
         * the external store (JSONL). Persist NOW because the
         * original entries it replaces are already gone — if
         * we skip this, the summary will never reach disk and the
         * next prime will find a gap. */
        if (s->on_l1_preserve) {
          const xAgentSessionMsg *base =
            (const xAgentSessionMsg *)xArrayData(s->history_arr);
          s->on_l1_preserve((xAgentSession)s, base + compact_start, 1,
                            xAgentL1PreserveReason_Compacted,
                            s->l1_preserve_owner);
        }
        /* The summary is now at index compact_start and is a
         * persisted entry. Ensure persisted_prefix covers it
         * so future flushes don't re-append it. Since entries
         * before compact_start were already persisted (they
         * were the head we preserved), the prefix should now
         * cover up to and including the summary. */
        size_t new_prefix = compact_start + 1;
        if (s->persisted_prefix < new_prefix) {
          s->persisted_prefix = new_prefix;
        }
      }
    }

    if (!compact_ok) {
      /* Compact failed (empty summary / OOM / provider error).
       * Leave history untouched — the caller decides whether to
       * retry, relax the budget, or surface an error. Silently
       * truncating here would violate the Summarize contract
       * (the user opted in precisely because they did NOT want to
       * lose the head outright), and would also risk an infinite
       * loop if the post-trim history is still over budget. */
      free(summary_text);
    }

    /* Reset compacting state. */
    s->compacting        = 0;
    s->compact_start_idx = 0;
    s->compact_end_idx   = 0;

    /* Invalidate incremental bookkeeping when history actually
     * changed (compact_ok). On failure history is intact, so the
     * bookkeeping is still valid. */
    if (compact_ok) {
      s->known_prompt_tokens = -1;
      s->delta_entries       = 0;
    }

    /* Free produced entries that the compact Query generated
     * (they're NOT merged into history — only the summary is). */
    for (size_t i = 0; i < n_produced; i++) {
      ai_session_msg_free(&produced[i]);
    }

    /* Destroy the compact Query BEFORE firing CompactDone.
     * xAgentQueryDestroy nulls s->query, and the auto-retry
     * below will re-enter xAgentSessionInput. If we kept s->query
     * pointing at the about-to-be-freed compact Query, the
     * admission check in xAgentSessionInput would still see it and
     * refuse with xErrno_Busy — defeating the whole point of the
     * event. */
    xAgentQueryDestroy(q);

    /* ── Notify caller: compact finished ──────────────────────
     * Fire CompactDone so the caller knows the session is now idle
     * and (if auto-retry is active) a new input is being submitted.
     * summary_ok distinguishes success (old entries replaced by a
     * summary) from failure (history unchanged). ────────────── */
    if (s->on_budget_event) {
      struct xAgentBudgetCompactDoneInfo cdi;
      cdi.summary_ok = compact_ok;
      cdi.summary_tokens =
        compact_ok
          ? ai_budget_estimate_tokens(
              (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
              1 /* just the summary entry */)
          : 0;
      cdi.entries_affected = compact_end;
      s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_CompactDone, &cdi,
                         s->budget_event_ud);
    }

    /* Auto-retry: if a pending user message was saved before the
     * compact was launched, re-submit it now. The budget gate will
     * re-evaluate; if still over budget the gate may trim more tool
     * results or refuse with PromptTooLong. In the rare case where
     * auto-retry fails, the pending message is lost and the caller
     * will need to retry manually. */
    session_auto_retry_pending_(s);

    /* The compact is done. The caller's on_done is NOT fired here
     * — they're still holding a Busy result. If auto-retry was
     * triggered, xAgentSessionInput has already been called and
     * will either start a new query or return an error. */
    return;
  }

  /* ── Normal (non-compact) on_done path ──────────────────────── */

  /* Private Query handle — needed for first_round_prompt_tokens. */
  struct xAgentQuery_ *q_priv = (struct xAgentQuery_ *)q;

  /* ── Incremental token bookkeeping ──────────────────────────────
   *
   * After a successful run we update the known prompt token baseline
   * using the provider-reported prompt_tokens from the FIRST round.
   * This value maps cleanly to the gate's pre-submit estimate in ALL
   * cases — single-round text conversations AND multi-round tool
   * loops alike.
   *
   * After this update, known_prompt_tokens is precise and
   * delta_entries is reset to 0. The next gate check will use
   * known_prompt_tokens as the baseline and only estimate the delta
   * of new entries added between now and the next Input. */
  if (usage && q_priv->first_round_prompt_tokens > 0) {
    s->known_prompt_tokens = q_priv->first_round_prompt_tokens;
    s->delta_entries       = 0;
  }
  /* Stash the first-round prompt_tokens for the next GatePassed
   * event — callers can display "estimated vs actual" without
   * the inflation that later tool-loop rounds would introduce. */
  s->last_first_round_prompt_tokens = q_priv->first_round_prompt_tokens;
  /* Reset last_gate_total for the next run. */
  s->last_gate_total = 0;

  /* Append every produced entry into history. Because the produced
   * array is now owned by xArray (which will release every element
   * when the Query is destroyed), we must deep-copy each entry's
   * heap strings rather than doing a bitwise move. */
  for (size_t i = 0; i < n_produced; i++) {
    struct xAgentSessionMsg_ *src = &produced[i];
    struct xAgentSessionMsg_ *dst = history_push(s);
    if (!dst)
      break; /* OOM: remaining produced entries stay for
              * xAgentQueryDestroy to clean up. */
    *dst = *src;
    /* Clear src so xArrayPop/release won't double-free the strings
     * we just moved. */
    memset(src, 0, sizeof(*src));
  }
  /* No free(produced) — the xArray owns the buffer. */

  /* Track the produced entries for incremental token bookkeeping.
   * Note: known_prompt_tokens was already updated above from the
   * first-round report, so these produced entries will be counted
   * as delta on the next gate check. */
  s->delta_entries += n_produced;

  if (s->cbs.on_done) {
    s->cbs.on_done((xAgentSession)s, reason, usage, s->cbs.user_data);
  }

  /* Main Query is done — clean up any sidecar state (idle timer,
   * sidecar Query, accumulated output). The sidecar is no longer
   * needed because the main Query's blocking tool has finished. */
  session_sidecar_cleanup(s);

  xAgentQueryDestroy(q);
}

/* One static callback set — stamped into every Query the Session
 * spawns, with user_data re-bound per-create to the Session handle. */
static const xAgentQueryCallbacks SESSION_FWD_CBS = {
  .on_text        = sess_fwd_on_text,
  .on_thinking    = sess_fwd_on_thinking,
  .on_done        = sess_fwd_on_done,
  .on_error       = sess_fwd_on_error,
  .on_tool        = sess_fwd_on_tool,
  .on_tool_output = sess_fwd_on_tool_output,
  .user_data      = NULL, /* filled in per-Query */
};

/* ── Public API ─────────────────────────────────────────────────────── */

xAgentSession xAgentSessionCreate(xAgent agent, const xAgentSessionConf *conf) {
  if (!agent || !conf) return NULL;

  struct xAgentSession_ *s = (struct xAgentSession_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  struct xAgent_ *a = (struct xAgent_ *)agent;

  s->agent = agent;
  s->cbs   = conf->cbs;

  s->system_prompt =
    conf->system_prompt ? conf->system_prompt : a->system_prompt;
  const char *model_src = conf->model ? conf->model : a->model;
  if (model_src) {
    s->model = strdup(model_src);
    if (!s->model) {
      free(s);
      return NULL;
    }
  } else {
    s->model = NULL;
  }
  s->max_turns  = conf->max_turns > 0 ? conf->max_turns : a->max_turns;
  s->max_tokens = conf->max_tokens > 0 ? conf->max_tokens : a->max_tokens;

  /* Structured budget config is a plain value copy: Disabled (the
   * zero default) keeps the session behaving exactly as before,
   * and the enforcement path is still dark at this point — c2/c3
   * will light it up. */
  s->budget = conf->budget;

  /* Budget-event callback is lifted out of budget conf into its
   * own slot for locality (avoids chasing budget. every time the
   * session fires an event). */
  s->on_budget_event = conf->budget.on_budget_event;
  s->budget_event_ud = conf->budget.budget_event_ud;

  /* Incremental token bookkeeping: known_prompt_tokens starts at
   * -1 (unknown / cold start). After the first successful run the
   * provider reports actual prompt_tokens which becomes the precise
   * baseline. delta_entries tracks how many history entries have
   * been added since the last provider report; only this small
   * delta needs coarse estimation. */
  s->known_prompt_tokens            = -1;
  s->delta_entries                  = 0;
  s->last_gate_total                = 0;
  s->last_first_round_prompt_tokens = -1;

  /* Pending auto-retry text: calloc zeroes these already,
   * but make the intent explicit. */
  s->pending_text     = NULL;
  s->pending_text_len = 0;

  /* Session-lifetime properties: stamped here, never mutated. Zero
   * for @c origin collapses to xAgentInputOrigin_User, which is also
   * the conservative default for callers who simply calloc the
   * conf. */
  s->origin           = conf->origin;
  s->on_finalizing    = conf->on_finalizing;
  s->finalizing_owner = conf->finalizing_owner;

  /* L1 memory-preservation hook: copied from conf for locality
   * (same pattern as on_budget_event). NULL = no L1 delivery. */
  s->on_l1_preserve    = conf->on_l1_preserve;
  s->l1_preserve_owner = conf->l1_preserve_owner;

  /* Session identifier: borrowed from conf, or auto-generated by
   * the agent in xAgentCreateSession. May be NULL. */
  s->session_id = conf->session_id;

  /* Sidecar idle-detection: zero (the default) disables the sidecar
   * mechanism entirely. A non-zero value means "if an async tool
   * call has not produced output for this many ms, launch a sidecar
   * Query so the AI can inspect and interact". */
  s->sidecar_idle_ms = conf->sidecar_idle_ms;

  /* @c s->query starts NULL (from calloc). A Query is allocated on
   * demand by xAgentSessionInput and released from sess_fwd_on_done. */

  /* Create the history array with a release callback that frees
   * per-element heap resources. No retain — callers fill the
   * zero-initialised slot manually after xArrayPush(). */
  s->history_arr =
    xArrayCreate(sizeof(struct xAgentSessionMsg_), 8, &kHistoryCbs);
  if (!s->history_arr) {
    free(s);
    return NULL;
  }

  return (xAgentSession)s;
}

xErrno xAgentSessionInput(xAgentSession sess, xAgentMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;
  struct xAgent_        *a = (struct xAgent_ *)s->agent;

  /* Single-flight: one live Query per Session. If the previous run
   * is still in flight refuse with Busy. Also refuse if a compact
   * (Summarize) Query is in flight — the caller must wait
   * for the compact to finish before submitting new input. */
  if (s->query || s->compacting) return xErrno_Busy;

  /* ── Per-turn memory retrieval ────────────────────────────────
   *
   * Ask the agent's memory store for context relevant to the
   * incoming user message. The hits are ephemeral — they ride
   * along in the Query's prompt for this one turn and are NOT
   * written back into history_arr, so they won't re-trigger their
   * own retrieval on the next turn and won't be double-persisted
   * by the L1 preserve hook.
   *
   * This is the first real caller that fills recent_turn /
   * budget_tokens in xAgentMemoryQuery. The built-in JSONL backend
   * treats recent_turn != NULL as "per-turn retrieval" and returns
   * an empty set (it has no index, and re-serving the tail would
   * duplicate what Create-time prime already injected). A future
   * vector / rerank backend is expected to do the real work here.
   *
   * Failure is opportunistic: if Retrieve errors out we silently
   * skip injection and still serve the turn on plain history.
   * This keeps the memory subsystem from ever blocking the user
   * input path. */
  xAgentMemoryHits hits;
  memset(&hits, 0, sizeof(hits));
  if (a->memory && s->session_id) {
    xAgentMemoryQuery rq;
    memset(&rq, 0, sizeof(rq));
    rq.session_id  = s->session_id;
    rq.recent_turn = &msg;
    /* budget_tokens / max_entries left at 0 — let the backend pick
     * sensible defaults until the session-level budget knob gets
     * properly plumbed through. */
    (void)xAgentMemoryRetrieve(a->memory, &rq, &hits);
  }

  /* Budget gate: consulted BEFORE any history mutation so the
   * Error policy can refuse without leaving partial state behind,
   * and the TruncateTail policy can shape history first so the
   * subsequent append lands on an already-conforming base. A
   * Disabled policy (the default) short-circuits inside
   * session_enforce_budget_() with zero measurable overhead. */
  xErrno rc = session_enforce_budget_(s, msg);
  if (rc != xErrno_Ok) {
    xAgentMemoryReleaseHits(a->memory, &hits);
    return rc;
  }

  /* Commit the user message to history first so the input view
   * below includes it. If the Query submit later fails we'll roll
   * this back. */
  size_t history_checkpoint = xArrayLen(s->history_arr);
  rc                        = history_append_user_msg(s, msg);
  if (rc != xErrno_Ok) {
    xAgentMemoryReleaseHits(a->memory, &hits);
    return rc;
  }
  /* Track the new entry for incremental token bookkeeping. */
  s->delta_entries += (xArrayLen(s->history_arr) - history_checkpoint);

  /* Build the complete message array the Query should run on
   * (system prompt + ephemeral memory hits + rolling history
   * including the new user turn). */
  struct sess_input_view_ view;
  rc = sess_input_view_build(s, &hits, /*hist_end=*/0, &view);
  if (rc != xErrno_Ok) {
    xAgentMemoryReleaseHits(a->memory, &hits);
    /* Roll back the user append — nothing observable happened. */
    while (xArrayLen(s->history_arr) > history_checkpoint) {
      xArrayPop(s->history_arr);
    }
    return rc;
  }

  /* view copied whatever it needed out of hits (the ephemeral_text
   * block is now self-contained in view), so we can release the
   * hits set before the Query run. This keeps the hits lifetime
   * short and tight. */
  xAgentMemoryReleaseHits(a->memory, &hits);

  /* Spawn a fresh Query with Session-level forwarding shims bound
   * to this Session. xAgentQueryCreate also sets s->query so a second
   * Input call during the same run hits the Busy branch above. */
  xAgentQueryConf qc = {0};
  qc.cbs             = SESSION_FWD_CBS;
  qc.cbs.user_data   = s;
  /* Only expose the confirmation gate if the host actually wired a
   * callback \u2014 otherwise leave on_tool_confirm NULL so needs_confirm
   * tools keep running without asking (backward-compatible default). */
  if (s->cbs.on_tool_confirm) {
    qc.cbs.on_tool_confirm = sess_fwd_on_tool_confirm;
  }
  /* Honour any per-session provider/model override; fall back to
   * the agent's defaults when none is set. See
   * xAgentSessionSetProvider() / xAgentSessionSetModel(). */
  qc.provider    = s->provider_override ? s->provider_override : a->provider;
  qc.tools       = (const xAgentTool **)a->tools;
  qc.tools_count = a->tools_count;
  qc.model       = s->model;
  qc.max_tokens  = s->max_tokens;
  qc.max_turns   = s->max_turns;
  qc.session     = sess;

  xAgentQuery q = xAgentQueryCreate(&qc);
  if (!q) {
    sess_input_view_free(&view);
    while (xArrayLen(s->history_arr) > history_checkpoint) {
      xArrayPop(s->history_arr);
    }
    return xErrno_NoMemory;
  }

  /* Hand the input array to the Query. xAgentQueryRun deep-copies
   * everything, so we can release @c view immediately afterwards
   * regardless of success/failure. */
  rc = xAgentQueryRun(q, view.msgs, view.n_msgs);
  sess_input_view_free(&view);

  if (rc != xErrno_Ok) {
    xAgentQueryDestroy(q); /* Clears s->query back to NULL. */
    /* No on_done fires on the failure path — roll back the user
     * message too so history reflects what actually happened. */
    while (xArrayLen(s->history_arr) > history_checkpoint) {
      xArrayPop(s->history_arr);
    }
    return rc;
  }
  return xErrno_Ok;
}

void xAgentSessionCancel(xAgentSession sess) {
  /* Thin wrapper over the query-level entry point. Both exist so
   * callers can target whichever layer their code already tracks;
   * the underlying semantics are identical (cancel this session's
   * single in-flight run). */
  xAgentQueryCancel(xAgentSessionQuery(sess));
}

void xAgentSessionSetProvider(xAgentSession sess, xAgentProvider provider) {
  /* Per the API contract this only affects the NEXT Query the
   * session launches (main or sidecar). Any in-flight Query keeps
   * streaming on the provider it was created with — we deliberately
   * do not touch s->query here. Internal maintenance Queries
   * (summary/compact) always use the agent's default provider and
   * ignore this override by design, so no special handling is
   * needed for them either. */
  if (!sess) return;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;
  s->provider_override     = provider;
}

xErrno xAgentSessionSetModel(xAgentSession sess, const char *model_id) {
  if (!sess) return xErrno_InvalidArg;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;
  struct xAgent_        *a = (struct xAgent_ *)s->agent;

  /* The registry path is the only way this API makes sense —
   * legacy single-provider agents have nothing to look up. */
  if (!a->model_registry) return xErrno_InvalidState;

  /* NULL id — revert to agent default model. */
  if (!model_id) {
    s->provider_override = NULL;
    free(s->model);
    s->model = a->model ? strdup(a->model) : NULL;
    return xErrno_Ok;
  }

  const xAgentModelSpec *spec =
    xAgentModelRegistryGet(a->model_registry, model_id);
  if (!spec) return xErrno_NotFound;

  /* Prepare the new model-name copy up front so a failed strdup
   * leaves the current model untouched. spec->model may legitimately
   * be NULL ("use the provider's own default") — in that case we
   * mirror it with a NULL model. */
  char *new_model = NULL;
  if (spec->model) {
    new_model = strdup(spec->model);
    if (!new_model) return xErrno_NoMemory;
  }

  s->provider_override = spec->provider;
  free(s->model);
  s->model = new_model;
  return xErrno_Ok;
}

void xAgentSessionSetContextWindow(xAgentSession sess, size_t context_window) {
  /* Only touch budget.context_window. Policy, callbacks,
   * etc. stay exactly as the session was configured at create time
   * — host apps that dial the window on a model switch shouldn't
   * have to re-specify the rest of the budget conf. The write is
   * consulted by session_budget_limit_() on the next
   * xAgentSessionInput; any Query already in flight keeps running
   * with the limit it was admitted under. */
  if (!sess) return;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;
  s->budget.context_window = context_window;
}

void xAgentSessionSetBudget(xAgentSession sess, const xAgentBudgetConf *conf) {
  /* Bulk variant of SetContextWindow. We deliberately copy ONLY the
   * context_window and leave s->budget.policy plus the
   * on_budget_event / budget_event_ud pair untouched — those are
   * infrastructure the host wired up at create time and would not
   * expect a "switch model" call to silently rewire. Anything in
   * flight keeps running under its admitted limits; the new values
   * take effect at the next session_budget_limit_() check. */
  if (!sess || !conf) return;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;
  s->budget.context_window = conf->context_window;
}

void xAgentSessionDestroy(xAgentSession sess) {
  if (!sess) return;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;

  /* Tear down the Query first (if any). xAgentQueryDestroy will
   * cancel-then-free synchronously; in the common "natural
   * completion" path sess_fwd_on_done has already destroyed it, so
   * this branch is mostly a safety net for mid-flight destroys. */
  if (s->query) {
    xAgentQueryDestroy((xAgentQuery)s->query);
    /* xAgentQueryDestroy nulls s->query for us. */
  }

  /* Clean up any sidecar state (sidecar Query, idle timer,
   * accumulated output chunks). Safe to call even when no sidecar
   * is active — it checks each field for NULL. */
  session_sidecar_cleanup(s);

  /* Fire the late-teardown hook while the session is still fully
   * live (history intact). Detach before calling so a misbehaving
   * hook that triggers a second destroy won't re-enter here. Per
   * contract the hook runs at most once. */

  /* L1 preserve: deliver the full remaining history as a final
   * snapshot before the session is torn down. This ensures
   * sessions that never triggered a budget event still deliver
   * their complete conversation to L1. Fires before
   * on_finalizing so the consumer sees the data first.
   *
   * When the session was primed from external memory, the leading
   * persisted_prefix entries are already in the store; skip them
   * here so we don't double-append on every resume. A session
   * that hasn't received any new turns since prime will end up
   * delivering an empty batch, which the callback must already
   * handle gracefully (the agent's own memory callback does). */
  if (s->on_l1_preserve) {
    xAgentSessionL1PreserveFunc hook  = s->on_l1_preserve;
    void                       *owner = s->l1_preserve_owner;
    s->on_l1_preserve                 = NULL;
    s->l1_preserve_owner              = NULL;
    size_t hist_len                   = xArrayLen(s->history_arr);
    size_t skip =
      s->persisted_prefix < hist_len ? s->persisted_prefix : hist_len;
    const xAgentSessionMsg *base =
      (const xAgentSessionMsg *)xArrayData(s->history_arr);
    /* Always invoke the hook on Finalizing so the owner can free
     * its context even when the emitted slice is empty.  Pass the
     * post-prefix tail; the callback is responsible for handling
     * n_msgs == 0 gracefully. */
    hook(sess, base + skip, hist_len - skip, xAgentL1PreserveReason_Finalizing,
         owner);
  }

  if (s->on_finalizing) {
    xAgentSessionFinalizingFunc hook  = s->on_finalizing;
    void                       *owner = s->finalizing_owner;
    s->on_finalizing                  = NULL;
    s->finalizing_owner               = NULL;
    hook(sess, owner);
  }

  /* xArrayDestroy calls the release callback (ai_session_msg_free)
   * for every element still in the array. */
  xArrayDestroy(s->history_arr);
  free(s->pending_text);
  free(s->model);
  free(s);
}

xAgentInputOrigin xAgentSessionOrigin(xAgentSession sess) {
  if (!sess) return xAgentInputOrigin_User;
  return ((struct xAgentSession_ *)sess)->origin;
}

const char *xAgentSessionId(xAgentSession sess) {
  if (!sess) return NULL;
  return ((struct xAgentSession_ *)sess)->session_id;
}
