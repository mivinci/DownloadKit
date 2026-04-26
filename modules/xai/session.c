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
 *   - Context-budget gate: before appending the user turn and
 *     creating a Query, xAiSessionInput runs the incoming msg +
 *     current history through the budget estimator (budget.c) and
 *     dispatches on xAiBudgetConf::policy. Error refuses with
 *     xErrno_PromptTooLong; TruncateOldest drops history entries
 *     at the earliest-keep boundary and, if that still does not
 *     fit, refuses. Disabled (the default) is a single-branch
 *     short-circuit and leaves behaviour byte-identical to
 *     pre-budget releases. Callback / SummarizeOldest policies
 *     are accepted by the parser but behave like Error until
 *     c4+ wires the real implementations.
 *   - Token-estimate calibration: each Session carries a tiny
 *     EWMA calibrator (budget_calibrator, budget_private.h) that
 *     corrects the static bytes/4 + envelope estimator against
 *     provider-reported xAiUsage.prompt_tokens. The gate consults
 *     the calibrated estimate; the on_done forwarder folds in one
 *     observation per clean (single-round, text-only) run. Runs
 *     with tool rounds are intentionally skipped because Query
 *     usage is accumulated across rounds and cannot be mapped
 *     back to a single-submit prompt size.
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
 *   - Budget-policy implementations for Callback (caller-supplied
 *     compaction) and SummarizeOldest (async summary query).
 *     Multi-round tool runs do not yet contribute calibration
 *     observations; a proper split of per-round usage out of
 *     Query would unlock that in a later commit.
 *   - Proper async teardown when destroy is called mid-flight.
 */

#include "session_private.h"

#include "agent_private.h"
#include "budget_private.h"

#include <xai/message.h>
#include <xai/provider.h>
#include <xai/query.h>
#include <xai/session.h>
#include <xbase/array.h>
#include <xbase/base.h>
#include <xbase/error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Forward declarations ──────────────────────────────────────────── */

static void sess_fwd_on_done(xAiQuery q, xAiDoneReason reason,
                             const xAiUsage *usage, void *ud);

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

/* xArray release callback for xAiSessionMsg_ elements. */
static void session_msg_release(void *elem) {
  ai_session_msg_free((struct xAiSessionMsg_ *)elem);
}

/* xArray callbacks for session history: release only (no retain, no equal).
 * Push returns a zero-initialised slot; the caller fills it manually. */
static const xArrayCallbacks kHistoryCbs = {NULL, session_msg_release, NULL};

/* Push a new zero-initialised slot onto the history array.
 * Returns the slot pointer, or NULL on allocation failure. */
static struct xAiSessionMsg_ *history_push(struct xAiSession_ *s) {
  return (struct xAiSessionMsg_ *)xArrayPush(&s->history_arr);
}

/* ── History append API (shared with query.c via session_private.h) ── */

xErrno ai_history_append_text(struct xAiSession_ *s, xAiRole role,
                              const char *text, size_t len) {
  struct xAiSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
  slot->kind = xAiSessionEntry_Text;
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

xErrno ai_history_append_tool_use(struct xAiSession_ *s, const char *id,
                                  const char *name, const char *args) {
  struct xAiSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role          = xAiRole_Assistant;
  slot->kind          = xAiSessionEntry_ToolUse;
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
xErrno ai_history_append_thinking(struct xAiSession_ *s, const char *text,
                                  size_t len) {
  struct xAiSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = xAiRole_Assistant;
  slot->kind = xAiSessionEntry_Thinking;
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

xErrno ai_history_append_tool_result(struct xAiSession_ *s, const char *id,
                                     const char *output, size_t output_len,
                                     int is_error) {
  struct xAiSessionMsg_ *slot = history_push(s);
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
    /* xArrayPop calls session_msg_release which frees the partial
     * fields we just allocated. */
    xArrayPop(s->history_arr);
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

  struct xAiSessionMsg_ *slot = history_push(s);
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

/* ── Context-budget enforcement ─────────────────────────────────────
 *
 * Gate xAiSessionInput runs on the structured xAiBudgetConf policy.
 * The pipeline runs BEFORE the user message is committed to
 * history, so the Error policy can refuse the turn without leaving
 * history polluted, and the TruncateOldest policy can shape history
 * first and then let the normal append path execute on an already-
 * conforming base.
 *
 * Wire picture:
 *
 *   xAiSessionInput(sess, msg)
 *     │
 *     ├─ budget.policy == Disabled   → skip, proceed as before
 *     ├─ budget.policy == Error      → estimate; if over, return
 *     │                                xErrno_PromptTooLong
 *     ├─ budget.policy == Truncate   → estimate; if over, trim
 *     │                                history front to the
 *     │                                earliest-keep boundary. If
 *     │                                still over afterwards fall
 *     │                                through to Error (refuse).
 *     └─ Callback / SummarizeOldest  → reserved; treated like Error
 *                                      until c4+ wires them up.
 *
 * The estimator is the coarse one from budget.c — bytes/4 plus a
 * per-entry envelope constant — and is intentionally conservative
 * so a slightly-overestimate still errs on the side of "refuse /
 * trim a bit more" rather than busting the real provider window.
 */

/* Estimate the token footprint of an incoming xAiMessage as if it
 * were already appended to history as a single xAiRole_User text
 * entry. Only text blocks contribute bytes (see
 * history_append_user_msg — non-text blocks on user input are
 * ignored), and the per-entry envelope constant is charged once
 * for the whole merged entry, matching how the append path creates
 * exactly one history slot. */
static size_t estimate_incoming_user_tokens_(xAiMessage msg) {
  size_t payload_bytes = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAiContentType_Text) {
      payload_bytes += msg.contents[i].u.text.len;
    }
  }
  return (payload_bytes / XAI_BUDGET_BYTES_PER_TOKEN) +
         XAI_BUDGET_PER_MSG_TOKENS;
}

/* Drop history entries @c [0, keep_idx) in place. Releases each
 * slot's owned strings via ai_session_msg_free() and shifts the
 * survivors down with memmove. The capacity is left untouched —
 * we expect the freed slots to be refilled by the very next
 * xAiSessionInput run, and shrinking the backing array would just
 * churn realloc. */
static void session_trim_history_front_(struct xAiSession_ *s,
                                        size_t              keep_idx) {
  if (keep_idx == 0 || keep_idx >= xArrayLen(s->history_arr)) return;
  xArrayRemoveRange(s->history_arr, 0, keep_idx);
}

/* Resolve the effective token ceiling for this session. A zero
 * @c max_tokens in the budget conf means "use the built-in
 * default"; callers that want a tighter or looser cap MUST set an
 * explicit value. Kept inline-ish so each policy branch does not
 * repeat the fallback. */
static size_t session_budget_limit_(const struct xAiSession_ *s) {
  return s->budget.max_tokens > 0 ? s->budget.max_tokens
                                  : XAI_BUDGET_DEFAULT_MAX_TOKENS;
}

/* Decide whether this incoming user message, combined with the
 * current history, fits in the configured budget — and if not,
 * act according to @c budget.policy. May mutate @c s->history
 * (TruncateOldest path).
 *
 * Returns xErrno_Ok to mean "proceed: it's safe to append and run
 * a Query"; any other value short-circuits xAiSessionInput.
 *
 * Note: the system_prompt is intentionally NOT counted here. It
 * is a borrowed, fixed-size sidecar that the trimmer cannot touch
 * anyway (invariant 1), so excluding it from the budget math keeps
 * the "can I trim enough to fit?" question honest: including it
 * would make an over-large system prompt look like regular history
 * pressure and yield no-op trims. */
/* ── Truncate oldest history and check budget ──────────────────
 *
 * Shared by the TruncateOldest case and the truncate_fallback
 * degradation path (SummarizeOldest failure). Returns xErrno_Ok
 * if trimming freed enough space, xErrno_PromptTooLong otherwise.
 * Fires xAiBudgetEvent_Truncated on success.
 */
static xErrno session_try_truncate_(struct xAiSession_ *s, size_t incoming,
                                    double factor, size_t limit) {
  size_t keep = ai_budget_earliest_keep(
    (const struct xAiSessionMsg_ *)xArrayData(s->history_arr),
    xArrayLen(s->history_arr), s->budget.keep_recent_turns);
  if (keep > 0) {
    session_trim_history_front_(s, keep);
    size_t current = ai_budget_estimate_tokens_calibrated(
      (const struct xAiSessionMsg_ *)xArrayData(s->history_arr),
      xArrayLen(s->history_arr), factor);
    if (current + incoming <= limit) {
      s->last_prompt_estimate = current + incoming;
      if (s->on_budget_event) {
        struct xAiBudgetTruncateInfo ti;
        ti.entries_removed = keep;
        s->on_budget_event((xAiSession)s, xAiBudgetEvent_Truncated, &ti,
                           s->budget_event_ud);
      }
      return xErrno_Ok;
    }
  }
  return xErrno_PromptTooLong;
}

static xErrno session_enforce_budget_(struct xAiSession_ *s, xAiMessage msg) {
  if (s->budget.policy == xAiBudgetPolicy_Disabled) return xErrno_Ok;

  size_t limit    = session_budget_limit_(s);
  size_t incoming = estimate_incoming_user_tokens_(msg);
  double factor   = s->budget_calibrator.factor;
  size_t current  = ai_budget_estimate_tokens_calibrated(
    (const struct xAiSessionMsg_ *)xArrayData(s->history_arr),
    xArrayLen(s->history_arr), factor);

  if (current + incoming <= limit) {
    /* Remember what the gate saw so sess_fwd_on_done can compare
     * it to the provider-reported prompt_tokens and update the
     * calibrator. Note we store the COMBINED number (history +
     * incoming), which is what the provider will actually count —
     * not just the history side. */
    s->last_prompt_estimate = current + incoming;
    return xErrno_Ok;
  }

  switch (s->budget.policy) {
  case xAiBudgetPolicy_TruncateOldest: {
    /* Ask the policy primitive for the earliest point we are
     * allowed to keep from while still honouring
     * keep_recent_turns. If that is 0 (floor exceeds what we
     * have, or no user turns to anchor on) we have nowhere to
     * trim — fall through to the refusal branch below. */
    return session_try_truncate_(s, incoming, factor, limit);
  }

  case xAiBudgetPolicy_Error:
    return xErrno_PromptTooLong;

  case xAiBudgetPolicy_Auto: {
    /* ── Auto: dynamically pick the best strategy ───────────────
     *
     * Decision heuristic based on the current history content:
     *
     *   1. Compute the tool-entry token ratio. When tool entries
     *      (ToolUse + ToolResult) dominate (≥ threshold),
     *      SummarizeOldest is a poor choice — LLMs cannot
     *      meaningfully compress structured JSON, and a bad
     *      summary may drop critical IDs or parameters.
     *      TruncateOldest is safer and faster in that regime.
     *
     *   2. When the conversation is predominantly text (ratio
     *      below threshold), SummarizeOldest has a good chance
     *      of preserving the gist, so we prefer it.
     *
     *   3. If SummarizeOldest is chosen but fails (OOM, provider
     *      error, empty summary), sess_fwd_on_done automatically
     *      degrades to TruncateOldest — no special handling here.
     */
    double ratio = ai_budget_tool_ratio(
      (const struct xAiSessionMsg_ *)xArrayData(s->history_arr),
      xArrayLen(s->history_arr));

    if (ratio >= XAI_BUDGET_AUTO_TOOL_RATIO_THRESHOLD) {
      /* Tool-heavy history → truncate is safer. */
      return session_try_truncate_(s, incoming, factor, limit);
    }
    /* Text-heavy history → try summarise (with truncate fallback
     * on failure). Fall through to SummarizeOldest logic. */
    goto auto_summarize;
  }

  case xAiBudgetPolicy_SummarizeOldest: {
  auto_summarize:;
    /* ── SummarizeOldest: compress old history into a summary ──
     *
     * The idea: instead of truncating history outright (losing
     * information), we launch a short internal Query that asks
     * the model to summarise the oldest portion of the
     * conversation. The summary replaces those entries, freeing
     * up budget space while preserving key facts.
     *
     * Steps:
     *   1. Find the earliest-keep boundary (same primitive as
     *      TruncateOldest).
     *   2. Bail if keep == 0 (nothing to summarise / floor
     *      exceeds history).
     *   3. If already compacting, return Busy.
     *   4. Build a summary system prompt + concatenate old
     *      messages as user content.
     *   5. Create an internal Query with budget_policy_override
     *      = Disabled (recursion guard).
     *   6. Run it.
     *   7. Return Busy — the caller waits for compact to
     *      finish; sess_fwd_on_done handles the rest.
     *
     * The compact completes asynchronously (the internal Query
     * runs on the same event loop). On completion,
     * sess_fwd_on_done detects the compacting state and
     * replaces the old history entries with the summary.
     */
    size_t keep = ai_budget_earliest_keep(
      (const struct xAiSessionMsg_ *)xArrayData(s->history_arr),
      xArrayLen(s->history_arr), s->budget.keep_recent_turns);

    /* Nothing to summarise or floor exceeds history. */
    if (keep == 0) return xErrno_PromptTooLong;

    /* Re-entrance guard: only one compact at a time. */
    if (s->compacting) return xErrno_Busy;

    /* Record the earliest index to keep after the compact
     * completes. Everything before this index will be replaced
     * by the summary. */
    s->compact_keep_idx = keep;

    /* ── Notify caller: compact is starting ───────────────
     * Fire the Compacting event BEFORE marking compacting=1 so
     * the caller can distinguish "Busy because compacting" from
     * "Busy because a normal Query is in flight". The info
     * struct tells them how many old entries are being compacted.
     * ────────────────────────────────────────────────────── */
    if (s->on_budget_event) {
      struct xAiBudgetCompactInfo ci;
      ci.entries_compacted = keep;
      s->on_budget_event((xAiSession)s, xAiBudgetEvent_Compacting, &ci,
                         s->budget_event_ud);
    }

    /* Build the summary system prompt with the message count. */
    char summary_sys[256];
    snprintf(summary_sys, sizeof(summary_sys), XAI_SUMMARY_SYSTEM_PROMPT, keep);

    /* Concatenate the old messages (entries [0, keep)) into one
     * user message string. We build a single text blob that the
     * model can summarise. */
    size_t old_bytes = 0;
    for (size_t i = 0; i < keep; i++) {
      struct xAiSessionMsg_ *m =
        (struct xAiSessionMsg_ *)xArrayAt(s->history_arr, i);
      if (m->text && m->text_len > 0) old_bytes += m->text_len;
      /* Rough separator between messages. */
      old_bytes += 2; /* "\n\n" */
    }
    char *old_text = NULL;
    if (old_bytes > 0) {
      old_text = (char *)calloc(old_bytes + 1, 1);
      if (!old_text) return xErrno_NoMemory;
      size_t off = 0;
      for (size_t i = 0; i < keep; i++) {
        struct xAiSessionMsg_ *m =
          (struct xAiSessionMsg_ *)xArrayAt(s->history_arr, i);
        if (m->text && m->text_len > 0) {
          memcpy(old_text + off, m->text, m->text_len);
          off += m->text_len;
        }
        if (i + 1 < keep) {
          memcpy(old_text + off, "\n\n", 2);
          off += 2;
        }
      }
    }

    /* Create an internal Query for the summary task.
     * - budget_policy_override = Disabled prevents the internal
     *   Query from triggering another budget check (recursion).
     * - The Query's only callback is on_done so we can harvest
     *   the summary text.
     * - We pass the summary system prompt + old text as the
     *   messages. */
    xAiQueryConf qc           = {0};
    qc.cbs.on_done            = sess_fwd_on_done;
    qc.cbs.user_data          = s;
    qc.budget_policy_override = xAiBudgetPolicy_Disabled;

    xAiQuery q = xAiQueryCreate((xAiSession)s, &qc);
    if (!q) {
      free(old_text);
      return xErrno_NoMemory;
    }

    /* Build the message array for the summary Query:
     *   1. System message with the summary prompt.
     *   2. User message with the concatenated old text. */
    xAiMessage msgs[2];
    xAiContent sys_blocks[1];
    xAiContent usr_blocks[1];

    /* System message. */
    sys_blocks[0].type        = xAiContentType_Text;
    sys_blocks[0].u.text.text = summary_sys;
    sys_blocks[0].u.text.len  = strlen(summary_sys);
    msgs[0].role              = xAiRole_System;
    msgs[0].contents          = sys_blocks;
    msgs[0].n                 = 1;

    /* User message with old conversation text. */
    usr_blocks[0].type        = xAiContentType_Text;
    usr_blocks[0].u.text.text = old_text ? old_text : "";
    usr_blocks[0].u.text.len  = old_text ? strlen(old_text) : 0;
    msgs[1].role              = xAiRole_User;
    msgs[1].contents          = usr_blocks;
    msgs[1].n                 = 1;

    /* Mark compacting before Run so the Session rejects new
     * inputs during the compact. */
    s->compacting             = 1;
    s->budget_policy_override = xAiBudgetPolicy_Disabled;

    xErrno rc = xAiQueryRun(q, msgs, 2);
    free(old_text);

    if (rc != xErrno_Ok) {
      /* Compact query failed to start — clean up and degrade to
       * TruncateOldest. */
      s->compacting             = 0;
      s->budget_policy_override = xAiBudgetPolicy_Disabled;
      xAiQueryDestroy(q);
      /* Fall through to TruncateOldest as degradation path. */
      goto truncate_fallback;
    }

    /* Compact query is now in flight. Return Busy so the caller
     * knows to wait. sess_fwd_on_done will handle the compact
     * completion. */
    return xErrno_Busy;
  }

  case xAiBudgetPolicy_Callback:
  case xAiBudgetPolicy_Disabled:
  default:
    /* Reserved policies are not implemented yet. Refuse by
     * default so a caller who asked for one does not silently
     * get Disabled behaviour — that could mask bugs for years.
     * c4+ replaces these arms with real wiring. */
    return xErrno_PromptTooLong;
  }

truncate_fallback:
  /* Degradation path: if SummarizeOldest fails (OOM, Query submit
   * error), fall back to TruncateOldest. This is the same logic as
   * the TruncateOldest case above but extracted as a goto target
   * for the SummarizeOldest branch to jump to on failure. */
  return session_try_truncate_(s, incoming, factor, limit);
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
  size_t hist_len = xArrayLen(s->history_arr);
  for (size_t i = 0; i < hist_len;) {
    struct xAiSessionMsg_ *m =
      (struct xAiSessionMsg_ *)xArrayAt(s->history_arr, i);
    if (m->role == xAiRole_Assistant) {
      size_t j = i;
      while (j < hist_len &&
             ((struct xAiSessionMsg_ *)xArrayAt(s->history_arr, j))->role ==
               xAiRole_Assistant)
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

  for (size_t i = 0; i < hist_len;) {
    struct xAiSessionMsg_ *m =
      (struct xAiSessionMsg_ *)xArrayAt(s->history_arr, i);
    if (m->role == xAiRole_Assistant) {
      size_t block_start = bi;
      size_t j           = i;
      while (j < hist_len) {
        struct xAiSessionMsg_ *mm =
          (struct xAiSessionMsg_ *)xArrayAt(s->history_arr, j);
        if (mm->role != xAiRole_Assistant) break;
        xAiContent *b = &out->blocks[bi++];
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

  /* ── SummarizeOldest compact completion ─────────────────────────
   *
   * When a compact (summary) Query completes, we need to:
   *   1. Extract the summary text from the produced entries.
   *   2. Replace the old history entries [0, compact_keep_idx)
   *      with one System summary entry.
   *   3. Re-check the budget — if still over, degrade to
   *      TruncateOldest.
   *   4. If budget is OK, the caller's original xAiSessionInput
   *      will be retried on the next call (we do NOT auto-retry
   *      here because the caller needs to know the compact
   *      happened).
   *
   * The compact on_done does NOT fire the caller's on_done —
   * this is an internal operation. The caller is still waiting
   * (they received xErrno_Busy from xAiSessionInput). */
  if (s->compacting) {
    /* Extract summary text: concatenate all text entries from
     * produced. */
    size_t summary_bytes = 0;
    for (size_t i = 0; i < n_produced; i++) {
      if (produced[i].kind == xAiSessionEntry_Text && produced[i].text) {
        summary_bytes += produced[i].text_len;
      }
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
          if (produced[i].kind == xAiSessionEntry_Text && produced[i].text) {
            memcpy(summary_text + off, produced[i].text, produced[i].text_len);
            off += produced[i].text_len;
          }
        }
      }
    }

    /* Compact succeeded (we got a non-empty summary) — replace
     * the old history entries with one System summary entry.
     * Compact failed (empty summary / OOM) — fall through to
     * TruncateOldest degradation. */
    size_t keep_idx   = s->compact_keep_idx;
    int    compact_ok = (summary_text != NULL && summary_bytes > 0);

    if (compact_ok) {
      /* Remove the old entries [0, keep_idx). */
      session_trim_history_front_(s, keep_idx);

      /* Insert the summary entry at the beginning of history. We
       * build it as a temporary xAiSessionMsg_ and splice it in
       * via xArrayInsert. */
      struct xAiSessionMsg_ summary_entry;
      memset(&summary_entry, 0, sizeof(summary_entry));
      summary_entry.role     = xAiRole_System;
      summary_entry.kind     = xAiSessionEntry_Text;
      summary_entry.text     = summary_text;
      summary_entry.text_len = prefix_len + summary_bytes;

      /* xArrayInsert shifts existing elements up and copies the
       * new element into position 0. */
      if (xArrayInsert(&s->history_arr, 0, &summary_entry) != xErrno_Ok) {
        /* OOM on insert — free the summary text and degrade. */
        free(summary_text);
        compact_ok = 0;
      } else {
        /* summary_text is now owned by the history array (it will
         * be freed by session_msg_release on removal). Clear the
         * local pointer so we don't double-free below. */
        summary_text = NULL;
      }
    }

    if (!compact_ok) {
      /* Degradation: truncate the same range we tried to
       * summarise. This matches the TruncateOldest behaviour. */
      free(summary_text);
      if (keep_idx > 0 && keep_idx < xArrayLen(s->history_arr)) {
        session_trim_history_front_(s, keep_idx);
      }
    }

    /* Reset compacting state. */
    s->compacting             = 0;
    s->compact_keep_idx       = 0;
    s->budget_policy_override = xAiBudgetPolicy_Disabled;

    /* ── Notify caller: compact finished ──────────────────────
     * Fire CompactDone so the caller knows the session is now idle
     * and can retry xAiSessionInput. summary_ok tells them whether
     * the old history was replaced by a summary or degraded to a
     * truncation. summary_tokens gives the token count of the new
     * summary entry (0 if no summary was produced). entries_affected
     * is the number of original entries that were replaced or
     * removed.
     * ────────────────────────────────────────────────────────── */
    if (s->on_budget_event) {
      struct xAiBudgetCompactDoneInfo cdi;
      cdi.summary_ok = compact_ok;
      cdi.summary_tokens =
        compact_ok
          ? ai_budget_estimate_tokens_calibrated(
              (const struct xAiSessionMsg_ *)xArrayData(s->history_arr),
              1, /* just the summary entry */
              s->budget_calibrator.factor)
          : 0;
      cdi.entries_affected = keep_idx;
      s->on_budget_event((xAiSession)s, xAiBudgetEvent_CompactDone, &cdi,
                         s->budget_event_ud);
    }

    /* Also fire Truncated if we degraded (compact_ok == 0 and we
     * actually truncated some entries). */
    if (!compact_ok && keep_idx > 0 && keep_idx < xArrayLen(s->history_arr)) {
      if (s->on_budget_event) {
        struct xAiBudgetTruncateInfo ti;
        ti.entries_removed = keep_idx;
        s->on_budget_event((xAiSession)s, xAiBudgetEvent_Truncated, &ti,
                           s->budget_event_ud);
      }
    }

    /* Free produced entries that the compact Query generated
     * (they're NOT merged into history — only the summary is). */
    for (size_t i = 0; i < n_produced; i++) {
      ai_session_msg_free(&produced[i]);
    }

    /* Destroy the compact Query. */
    xAiQueryDestroy(q);

    /* The compact is done. The caller's on_done is NOT fired here
     * — they're still holding a Busy result. The next time they
     * call xAiSessionInput, the budget gate will see the
     * (hopefully reduced) history and either let the turn through
     * or apply further policy. */
    return;
  }

  /* ── Normal (non-compact) on_done path ──────────────────────── */

  /* Calibration opt-in check — done BEFORE we move produced into
   * history so the scan is cheap (small, local array) and
   * independent of future history growth.
   *
   * Rationale for "single round only": Query.usage is accumulated
   * across every provider round (see query.c:usage_accumulate —
   * each round's prompt_tokens counts the cumulative inputs +
   * prior tool_results the model saw on that round). Feeding that
   * sum into a calibrator that estimates a SINGLE-submit prompt
   * would inflate the factor by a factor-of-rounds each time and
   * make the gate pathologically conservative after a few tool
   * sessions. Detecting single-round is cheap and reliable: a
   * tool_use in produced means there was at least a second round
   * to deliver the tool_result. No tool_use → exactly one round
   * → usage->prompt_tokens maps cleanly to last_prompt_estimate.
   *
   * Other opt-outs handled inside ai_budget_calibrator_update():
   *   - usage == NULL or prompt_tokens <= 0 (unknown)
   *   - last_prompt_estimate == 0 (gate was Disabled or not run) */
  int single_round = 1;
  for (size_t i = 0; i < n_produced; i++) {
    if (produced[i].kind == xAiSessionEntry_ToolUse) {
      single_round = 0;
      break;
    }
  }
  if (single_round && usage && s->last_prompt_estimate > 0) {
    ai_budget_calibrator_update(&s->budget_calibrator, s->last_prompt_estimate,
                                usage->prompt_tokens);
  }
  /* Reset for the next run either way — a stale estimate crossing
   * query boundaries would be a foot-gun if some future code path
   * fires on_done without having gone through the gate. */
  s->last_prompt_estimate = 0;

  /* Append every produced entry into history. Because the produced
   * array is now owned by xArray (which will release every element
   * when the Query is destroyed), we must deep-copy each entry's
   * heap strings rather than doing a bitwise move. */
  for (size_t i = 0; i < n_produced; i++) {
    struct xAiSessionMsg_ *src = &produced[i];
    struct xAiSessionMsg_ *dst = history_push(s);
    if (!dst)
      break; /* OOM: remaining produced entries stay for
              * xAiQueryDestroy to clean up. */
    *dst = *src;
    /* Clear src so xArrayPop/release won't double-free the strings
     * we just moved. */
    memset(src, 0, sizeof(*src));
  }
  /* No free(produced) — the xArray owns the buffer. */

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

  s->system_prompt =
    conf->system_prompt ? conf->system_prompt : a->system_prompt;
  s->model      = conf->model ? conf->model : a->model;
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

  /* Token-estimate calibrator boots at identity (factor = 1.0);
   * it accumulates observations from sess_fwd_on_done on clean
   * single-round runs. last_prompt_estimate is zero until the
   * first gate run records one. */
  ai_budget_calibrator_init(&s->budget_calibrator);
  s->last_prompt_estimate = 0;

  /* Session-lifetime properties: stamped here, never mutated. Zero
   * for @c origin collapses to xAiInputOrigin_User, which is also
   * the conservative default for callers who simply calloc the
   * conf. */
  s->origin           = conf->origin;
  s->on_finalizing    = conf->on_finalizing;
  s->finalizing_owner = conf->finalizing_owner;

  /* @c s->query starts NULL (from calloc). A Query is allocated on
   * demand by xAiSessionInput and released from sess_fwd_on_done. */

  /* Create the history array with a release callback that frees
   * per-element heap resources. No retain — callers fill the
   * zero-initialised slot manually after xArrayPush(). */
  s->history_arr = xArrayCreate(sizeof(struct xAiSessionMsg_), 8, &kHistoryCbs);
  if (!s->history_arr) {
    free(s);
    return NULL;
  }

  return (xAiSession)s;
}

xErrno xAiSessionInput(xAiSession sess, xAiMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAiSession_ *s = (struct xAiSession_ *)sess;

  /* Single-flight: one live Query per Session. If the previous run
   * is still in flight refuse with Busy. Also refuse if a compact
   * (SummarizeOldest) Query is in flight — the caller must wait
   * for the compact to finish before submitting new input. */
  if (s->query || s->compacting) return xErrno_Busy;

  /* Budget gate: consulted BEFORE any history mutation so the
   * Error policy can refuse without leaving partial state behind,
   * and the TruncateOldest policy can shape history first so the
   * subsequent append lands on an already-conforming base. A
   * Disabled policy (the default) short-circuits inside
   * session_enforce_budget_() with zero measurable overhead. */
  xErrno rc = session_enforce_budget_(s, msg);
  if (rc != xErrno_Ok) return rc;

  /* Commit the user message to history first so the input view
   * below includes it. If the Query submit later fails we'll roll
   * this back. */
  size_t history_checkpoint = xArrayLen(s->history_arr);
  rc                        = history_append_user_msg(s, msg);
  if (rc != xErrno_Ok) return rc;

  /* Build the complete message array the Query should run on
   * (system prompt + rolling history including the new user turn). */
  struct sess_input_view_ view;
  rc = sess_input_view_build(s, &view);
  if (rc != xErrno_Ok) {
    /* Roll back the user append — nothing observable happened. */
    while (xArrayLen(s->history_arr) > history_checkpoint) {
      xArrayPop(s->history_arr);
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
    while (xArrayLen(s->history_arr) > history_checkpoint) {
      xArrayPop(s->history_arr);
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
    while (xArrayLen(s->history_arr) > history_checkpoint) {
      xArrayPop(s->history_arr);
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
    xAiSessionFinalizingFunc hook  = s->on_finalizing;
    void                    *owner = s->finalizing_owner;
    s->on_finalizing               = NULL;
    s->finalizing_owner            = NULL;
    hook(sess, owner);
  }

  /* xArrayDestroy calls the release callback (ai_session_msg_free)
   * for every element still in the array. */
  xArrayDestroy(s->history_arr);
  free(s);
}

xAiInputOrigin xAiSessionOrigin(xAiSession sess) {
  if (!sess) return xAiInputOrigin_User;
  return ((struct xAiSession_ *)sess)->origin;
}
