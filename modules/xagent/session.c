/*
 * Copyright 2025 The xKit Authors. All rights reserved.
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
 *     provider-reported xAgentUsage.prompt_tokens. The gate consults
 *     the calibrated estimate; the on_done forwarder folds in one
 *     observation per clean (single-round, text-only) run. Runs
 *     with tool rounds are intentionally skipped because Query
 *     usage is accumulated across rounds and cannot be mapped
 *     back to a single-submit prompt size.
 *
 * The provider / tool loop itself lives in query.c; the Session
 * installs a static set of forwarding callbacks that re-dispatch
 * Query-level events (on_text, on_thinking, on_done, ...) to the
 * caller's xAgentSessionCallbacks with the Session handle. The Query
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
#include "tool_private.h"

#include <xagent/message.h>
#include <xagent/provider.h>
#include <xagent/query.h>
#include <xagent/session.h>
#include <xbase/array.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/time.h>

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
 * Returns the slot pointer, or NULL on allocation failure. */
static struct xAgentSessionMsg_ *history_push(struct xAgentSession_ *s) {
  return (struct xAgentSessionMsg_ *)xArrayPush(&s->history_arr);
}

/* ── History append API (shared with query.c via session_private.h) ── */

xErrno ai_history_append_text(struct xAgentSession_ *s, xAgentRole role,
                              const char *text, size_t len) {
  struct xAgentSessionMsg_ *slot = history_push(s);
  if (!slot) return xErrno_NoMemory;
  slot->role = role;
  slot->kind = xAgentSessionEntry_Text;
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
static xErrno history_append_user_msg(struct xAgentSession_ *s, xAgentMessage msg) {
  size_t total = 0;
  for (size_t i = 0; i < msg.n; i++) {
    if (msg.contents[i].type == xAgentContentType_Text) {
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
 * history polluted, and the TruncateOldest policy can shape history
 * first and then let the normal append path execute on an already-
 * conforming base.
 *
 * Wire picture:
 *
 *   xAgentSessionInput(sess, msg)
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

/* Estimate the token footprint of an incoming xAgentMessage as if it
 * were already appended to history as a single xAgentRole_User text
 * entry. Only text blocks contribute bytes (see
 * history_append_user_msg — non-text blocks on user input are
 * ignored), and the per-entry envelope constant is charged once
 * for the whole merged entry, matching how the append path creates
 * exactly one history slot. */
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

/* Drop history entries @c [0, keep_idx) in place. Releases each
 * slot's owned strings via ai_session_msg_free() and shifts the
 * survivors down with memmove. The capacity is left untouched —
 * we expect the freed slots to be refilled by the very next
 * xAgentSessionInput run, and shrinking the backing array would just
 * churn realloc. */
static void session_trim_history_front_(struct xAgentSession_ *s,
                                        size_t              keep_idx) {
  if (keep_idx == 0 || keep_idx >= xArrayLen(s->history_arr)) return;
  xArrayRemoveRange(s->history_arr, 0, keep_idx);
}

/* Resolve the effective token ceiling for this session. A zero
 * @c max_tokens in the budget conf means "use the built-in
 * default"; callers that want a tighter or looser cap MUST set an
 * explicit value. Kept inline-ish so each policy branch does not
 * repeat the fallback. */
static size_t session_budget_limit_(const struct xAgentSession_ *s) {
  return s->budget.max_tokens > 0 ? s->budget.max_tokens
                                  : XAGENT_BUDGET_DEFAULT_MAX_TOKENS;
}

/* Decide whether this incoming user message, combined with the
 * current history, fits in the configured budget — and if not,
 * act according to @c budget.policy. May mutate @c s->history
 * (TruncateOldest path).
 *
 * Returns xErrno_Ok to mean "proceed: it's safe to append and run
 * a Query"; any other value short-circuits xAgentSessionInput.
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
 * Fires xAgentBudgetEvent_Truncated on success.
 */
static xErrno session_try_truncate_(struct xAgentSession_ *s, size_t incoming,
                                    double factor, size_t limit) {
  size_t keep = ai_budget_earliest_keep(
    (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
    xArrayLen(s->history_arr), s->budget.keep_recent_turns);
  if (keep > 0) {
    /* L1 preserve: deliver the about-to-be-dropped entries [0, keep)
     * before they are permanently lost. The callback receives a
     * read-only slice — it must deep-copy anything it wants to
     * retain. This fires BEFORE the actual trim so the entries
     * are still valid in the history array. */
    if (s->on_l1_preserve) {
      s->on_l1_preserve((xAgentSession)s,
                        (const xAgentSessionMsg *)xArrayData(s->history_arr), keep,
                        xAgentL1PreserveReason_Truncated, s->l1_preserve_owner);
    }
    session_trim_history_front_(s, keep);
    size_t current = ai_budget_estimate_tokens_calibrated(
      (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
      xArrayLen(s->history_arr), factor);
    if (current + incoming <= limit) {
      s->last_prompt_estimate = current + incoming;
      if (s->on_budget_event) {
        struct xAgentBudgetTruncateInfo ti;
        ti.entries_removed = keep;
        s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_Truncated, &ti,
                           s->budget_event_ud);
      }
      return xErrno_Ok;
    }
  }
  return xErrno_PromptTooLong;
}

static xErrno session_enforce_budget_(struct xAgentSession_ *s, xAgentMessage msg) {
  if (s->budget.policy == xAgentBudgetPolicy_Disabled) return xErrno_Ok;

  size_t limit    = session_budget_limit_(s);
  size_t incoming = estimate_incoming_user_tokens_(msg);
  double factor   = s->budget_calibrator.factor;
  size_t current  = ai_budget_estimate_tokens_calibrated(
    (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
    xArrayLen(s->history_arr), factor);

  if (current + incoming <= limit) {
    /* Remember what the gate saw so sess_fwd_on_done can compare
     * it to the provider-reported prompt_tokens and update the
     * calibrator. Note we store the COMBINED number (history +
     * incoming), which is what the provider will actually count —
     * not just the history side. */
    s->last_prompt_estimate = current + incoming;

    /* Notify the caller that the gate passed, including the
     * token breakdown so they can display remaining capacity. */
    if (s->on_budget_event) {
      struct xAgentBudgetGateInfo gi;
      gi.limit                          = limit;
      gi.estimated                      = current + incoming;
      gi.remaining                      = limit - gi.estimated;
      gi.calibrator_factor              = s->budget_calibrator.factor;
      gi.calibrator_samples             = s->budget_calibrator.samples;
      gi.last_first_round_prompt_tokens = s->last_first_round_prompt_tokens;
      s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_GatePassed, &gi,
                         s->budget_event_ud);
    }

    return xErrno_Ok;
  }

  switch (s->budget.policy) {
  case xAgentBudgetPolicy_TruncateOldest: {
    /* Ask the policy primitive for the earliest point we are
     * allowed to keep from while still honouring
     * keep_recent_turns. If that is 0 (floor exceeds what we
     * have, or no user turns to anchor on) we have nowhere to
     * trim — fall through to the refusal branch below. */
    return session_try_truncate_(s, incoming, factor, limit);
  }

  case xAgentBudgetPolicy_Error:
    return xErrno_PromptTooLong;

  case xAgentBudgetPolicy_Auto: {
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
      (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
      xArrayLen(s->history_arr));

    if (ratio >= XAGENT_BUDGET_AUTO_TOOL_RATIO_THRESHOLD) {
      /* Tool-heavy history → truncate is safer. */
      return session_try_truncate_(s, incoming, factor, limit);
    }
    /* Text-heavy history → try summarise (with truncate fallback
     * on failure). Fall through to SummarizeOldest logic. */
    goto auto_summarize;
  }

  case xAgentBudgetPolicy_SummarizeOldest: {
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
     *   5. Create an internal Query (budget enforcement is
     *      implicitly disabled because the compact Query is
     *      driven by session_enforce_budget_ which gates on
     *      s->compacting).
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
      (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
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
      struct xAgentBudgetCompactInfo ci;
      ci.entries_compacted = keep;
      s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_Compacting, &ci,
                         s->budget_event_ud);
    }

    /* Build the summary system prompt with the message count. */
    char summary_sys[256];
    snprintf(summary_sys, sizeof(summary_sys), XAGENT_SUMMARY_SYSTEM_PROMPT, keep);

    /* Concatenate the old messages (entries [0, keep)) into one
     * user message string. We build a single text blob that the
     * model can summarise. */
    size_t old_bytes = 0;
    for (size_t i = 0; i < keep; i++) {
      struct xAgentSessionMsg_ *m =
        (struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, i);
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
        struct xAgentSessionMsg_ *m =
          (struct xAgentSessionMsg_ *)xArrayAt(s->history_arr, i);
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
     * - The Query's only callback is on_done so we can harvest
     *   the summary text.
     * - We pass the summary system prompt + old text as the
     *   messages.
     * - Budget enforcement is implicitly disabled: the compact
     *   Query is created by session_enforce_budget_ which gates
     *   on s->compacting, so no recursive budget check occurs. */
    struct xAgent_ *a  = (struct xAgent_ *)s->agent;
    xAgentQueryConf      qc = {0};
    qc.cbs.on_done       = sess_fwd_on_done;
    qc.cbs.user_data     = s;
    qc.provider          = a->provider;
    qc.tools             = (const xAgentTool **)a->tools;
    qc.tools_count       = a->tools_count;
    qc.model             = s->model;
    qc.max_tokens        = s->max_tokens;
    qc.session           = (xAgentSession)s;

    xAgentQuery q = xAgentQueryCreate(&qc);
    if (!q) {
      free(old_text);
      return xErrno_NoMemory;
    }

    /* Build the message array for the summary Query:
     *   1. System message with the summary prompt.
     *   2. User message with the concatenated old text. */
    xAgentMessage msgs[2];
    xAgentContent sys_blocks[1];
    xAgentContent usr_blocks[1];

    /* System message. */
    sys_blocks[0].type        = xAgentContentType_Text;
    sys_blocks[0].u.text.text = summary_sys;
    sys_blocks[0].u.text.len  = strlen(summary_sys);
    msgs[0].role              = xAgentRole_System;
    msgs[0].contents          = sys_blocks;
    msgs[0].n                 = 1;

    /* User message with old conversation text. */
    usr_blocks[0].type        = xAgentContentType_Text;
    usr_blocks[0].u.text.text = old_text ? old_text : "";
    usr_blocks[0].u.text.len  = old_text ? strlen(old_text) : 0;
    msgs[1].role              = xAgentRole_User;
    msgs[1].contents          = usr_blocks;
    msgs[1].n                 = 1;

    /* Mark compacting before Run so the Session rejects new
     * inputs during the compact. */
    s->compacting = 1;

    xErrno rc = xAgentQueryRun(q, msgs, 2);
    free(old_text);

    if (rc != xErrno_Ok) {
      /* Compact query failed to start — clean up and degrade to
       * TruncateOldest. */
      s->compacting = 0;
      xAgentQueryDestroy(q);
      /* Fall through to TruncateOldest as degradation path. */
      goto truncate_fallback;
    }

    /* Compact query is now in flight. Return Busy so the caller
     * knows to wait. sess_fwd_on_done will handle the compact
     * completion. */
    return xErrno_Busy;
  }

  case xAgentBudgetPolicy_Callback:
  case xAgentBudgetPolicy_Disabled:
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
 * Every xAgentSessionInput run hands the Query a complete, self-
 * contained message array: (optional) System prompt + the rolling
 * history the Session already has, including the user message we
 * just appended. Each xAgentMessage borrows from session-owned storage
 * (history entries live until the run terminates or the Session is
 * torn down) — the Query still deep-copies, so once xAgentQueryRun
 * returns this transient array can go away.
 */

struct sess_input_view_ {
  xAgentMessage *msgs;
  xAgentContent *blocks;
  size_t      n_msgs;
  size_t      n_blocks;
};

static void sess_input_view_free(struct sess_input_view_ *v) {
  free(v->msgs);
  free(v->blocks);
  memset(v, 0, sizeof(*v));
}

/* Build a message array from the current session state. Consecutive
 * Assistant entries are folded into one xAgentMessage (so thinking +
 * text + tool_use blocks travel together); other roles map 1:1. */
static xErrno sess_input_view_build(struct xAgentSession_      *s,
                                    struct sess_input_view_ *out) {
  memset(out, 0, sizeof(*out));
  size_t extra_system = (s->system_prompt && s->system_prompt[0]) ? 1 : 0;

  /* Pass 1: count. */
  size_t n_msgs   = extra_system;
  size_t n_blocks = extra_system;
  size_t hist_len = xArrayLen(s->history_arr);
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
    sess_input_view_free(out);
    return xErrno_NoMemory;
  }
  out->n_msgs   = n_msgs;
  out->n_blocks = n_blocks;

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
  if (s->sidecar_idle_ms > 0 && a->enable_sidecar_query
      && s->query && !s->sidecar) {

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
                                     const char *tool_use_id,
                                     const char *args_json,
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
#define XAGENT_SIDECAR_SYSTEM_PROMPT                                           \
  "You are a diagnostic assistant. A tool call is blocking the main "       \
  "conversation because it has not produced output for a while. "           \
  "You are given the conversation history above for context so you "        \
  "can understand what the user asked and what the assistant was "          \
  "doing. Analyse the tool output shown below and decide whether to "       \
  "send input to the running command. If the command is waiting for "       \
  "user input (e.g. a prompt, confirmation, or REPL), use the "            \
  "shell_stdin tool to send the appropriate input. If the command "         \
  "appears to be genuinely running (e.g. compiling, downloading), "        \
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
    size_t                 n_produced = 0;
    ai_query_take_produced((struct xAgentQuery_ *)q, &produced, &n_produced);

    for (size_t i = 0; i < n_produced; i++) {
      if (produced[i].kind == xAgentSessionEntry_Text && produced[i].text &&
          produced[i].text_len > 0) {
        /* Sidecar text is informational — just forward it. */
        if (s->cbs.on_text) {
          s->cbs.on_text((xAgentSession)s, produced[i].text, produced[i].text_len,
                         s->cbs.user_data);
        }
      }
    }
  }

  /* Notify the caller that the sidecar query has completed. */
  if (s->cbs.on_sidecar) {
    s->cbs.on_sidecar((xAgentSession)s, xAgentSidecarEvent_Done, s->cbs.user_data);
  }

  /* Clean up the sidecar state. */
  session_sidecar_cleanup(s);
}

/* Timer callback: the main Query's async tool has been idle for
 * sidecar_idle_ms. Launch a sidecar Query so the AI can inspect
 * the situation. */
static void session_sidecar_idle_timer_cb(void *arg) {
  struct xAgentSession_ *s = (struct xAgentSession_ *)arg;
  s->sidecar_idle_timer = NULL; /* timer has fired, handle is stale */

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

  const char *tid = s->sidecar_tool_use_id ? s->sidecar_tool_use_id : "(unknown)";
  size_t tid_len = strlen(tid);
  size_t prefix_len = sizeof(kIdlePrefix) - 1;
  size_t mid_len = sizeof(kIdleMid) - 1;
  size_t user_len = prefix_len + tid_len + mid_len + total_len;

  char *user_text = (char *)malloc(user_len + 1);
  if (!user_text) {
    free(output_text);
    return;
  }
  size_t uoff = 0;
  memcpy(user_text + uoff, kIdlePrefix, prefix_len); uoff += prefix_len;
  memcpy(user_text + uoff, tid, tid_len); uoff += tid_len;
  memcpy(user_text + uoff, kIdleMid, mid_len); uoff += mid_len;
  if (output_text && total_len > 0) {
    memcpy(user_text + uoff, output_text, total_len); uoff += total_len;
  }
  user_text[uoff] = '\0';
  free(output_text);

  /* Build the session's conversation history view so the sidecar
   * has context about the user's original request and the assistant's
   * actions so far. */
  struct sess_input_view_ hist_view;
  xErrno vrc = sess_input_view_build(s, &hist_view);
  if (vrc != xErrno_Ok) {
    free(user_text);
    return;
  }

  /* Determine whether the history view starts with a system prompt
   * (we'll skip it and use the sidecar-specific one instead). */
  int has_system = (hist_view.n_msgs > 0 &&
                    hist_view.msgs[0].role == xAgentRole_System);
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
  size_t sidecar_tools_count = 0;
  if (stdin_tool) {
    sidecar_tools[0]     = stdin_tool;
    sidecar_tools_count  = 1;
  }

  /* ── Allocate the combined message array ───────────────────────
   * Layout: [sidecar_system] + [history_msgs (minus original system)]
   *       + [idle_user]
   * We always emit our own sidecar system prompt; if the history
   * view contained one it is skipped (hist_skip == 1). */
  size_t n_hist = hist_view.n_msgs - hist_skip;
  /* +1 for the sidecar system msg + +1 for the idle user message. */
  size_t n_total_msgs = 1 + n_hist + 1;

  xAgentMessage *msgs = (xAgentMessage *)calloc(n_total_msgs, sizeof(xAgentMessage));
  xAgentContent *extra_blocks = (xAgentContent *)calloc(2, sizeof(xAgentContent));
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
  msgs[mi].role     = xAgentRole_System;
  msgs[mi].contents = &extra_blocks[0];
  msgs[mi].n        = 1;
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
  msgs[mi].role     = xAgentRole_User;
  msgs[mi].contents = &extra_blocks[1];
  msgs[mi].n        = 1;
  mi++;

  /* ── Create and run the sidecar Query ──────────────────────── */
  xAgentQueryConf qc = {0};
  qc.cbs.on_done   = session_sidecar_on_done;
  qc.cbs.user_data = s;
  qc.provider      = a->provider;
  qc.tools         = sidecar_tools_count > 0 ? sidecar_tools : NULL;
  qc.tools_count   = sidecar_tools_count;
  qc.model         = s->model;
  qc.max_tokens    = 256;  /* sidecar should be concise */
  qc.max_turns     = 1;    /* single round: analyse + act */
  qc.session       = (xAgentSession)s;

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
    s->cbs.on_sidecar((xAgentSession)s, xAgentSidecarEvent_Started, s->cbs.user_data);
  }
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
  size_t                 n_produced = 0;
  ai_query_take_produced((struct xAgentQuery_ *)q, &produced, &n_produced);

  /* ── SummarizeOldest compact completion ─────────────────────────
   *
   * When a compact (summary) Query completes, we need to:
   *   1. Extract the summary text from the produced entries.
   *   2. Replace the old history entries [0, compact_keep_idx)
   *      with one System summary entry.
   *   3. Re-check the budget — if still over, degrade to
   *      TruncateOldest.
   *   4. If budget is OK, the caller's original xAgentSessionInput
   *      will be retried on the next call (we do NOT auto-retry
   *      here because the caller needs to know the compact
   *      happened).
   *
   * The compact on_done does NOT fire the caller's on_done —
   * this is an internal operation. The caller is still waiting
   * (they received xErrno_Busy from xAgentSessionInput). */
  if (s->compacting) {
    /* Extract summary text: concatenate all text entries from
     * produced. */
    size_t summary_bytes = 0;
    for (size_t i = 0; i < n_produced; i++) {
      if (produced[i].kind == xAgentSessionEntry_Text && produced[i].text) {
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
          if (produced[i].kind == xAgentSessionEntry_Text && produced[i].text) {
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
      /* L1 preserve: deliver the about-to-be-replaced entries [0, keep_idx)
       * before they are swapped out by the summary. The consumer may want
       * the original full-fidelity entries even though a summary will take
       * their place in the session's history. This fires BEFORE the trim. */
      if (s->on_l1_preserve && keep_idx > 0) {
        s->on_l1_preserve(
          (xAgentSession)s, (const xAgentSessionMsg *)xArrayData(s->history_arr),
          keep_idx, xAgentL1PreserveReason_Compacted, s->l1_preserve_owner);
      }
      /* Remove the old entries [0, keep_idx). */
      session_trim_history_front_(s, keep_idx);

      /* Insert the summary entry at the beginning of history. We
       * build it as a temporary xAgentSessionMsg_ and splice it in
       * via xArrayInsert. */
      struct xAgentSessionMsg_ summary_entry;
      memset(&summary_entry, 0, sizeof(summary_entry));
      summary_entry.role     = xAgentRole_System;
      summary_entry.kind     = xAgentSessionEntry_Text;
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
    s->compacting       = 0;
    s->compact_keep_idx = 0;

    /* Free produced entries that the compact Query generated
     * (they're NOT merged into history — only the summary is). */
    for (size_t i = 0; i < n_produced; i++) {
      ai_session_msg_free(&produced[i]);
    }

    /* Destroy the compact Query BEFORE firing CompactDone/Truncated.
     * xAgentQueryDestroy nulls s->query, and the caller is expected to
     * re-enter xAgentSessionInput from inside the CompactDone callback
     * (auto-retry pattern). If we kept s->query pointing at the
     * about-to-be-freed compact Query while the callback ran, the
     * admission check in xAgentSessionInput would still see it and
     * refuse with xErrno_Busy — defeating the whole point of the
     * event. */
    xAgentQueryDestroy(q);

    /* ── Notify caller: compact finished ──────────────────────
     * Fire CompactDone so the caller knows the session is now idle
     * and can retry xAgentSessionInput. summary_ok tells them whether
     * the old history was replaced by a summary or degraded to a
     * truncation. summary_tokens gives the token count of the new
     * summary entry (0 if no summary was produced). entries_affected
     * is the number of original entries that were replaced or
     * removed.
     * ────────────────────────────────────────────────────────── */
    if (s->on_budget_event) {
      struct xAgentBudgetCompactDoneInfo cdi;
      cdi.summary_ok = compact_ok;
      cdi.summary_tokens =
        compact_ok
          ? ai_budget_estimate_tokens_calibrated(
              (const struct xAgentSessionMsg_ *)xArrayData(s->history_arr),
              1, /* just the summary entry */
              s->budget_calibrator.factor)
          : 0;
      cdi.entries_affected = keep_idx;
      s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_CompactDone, &cdi,
                         s->budget_event_ud);
    }

    /* Also fire Truncated if we degraded (compact_ok == 0 and we
     * actually truncated some entries). */
    if (!compact_ok && keep_idx > 0 && keep_idx < xArrayLen(s->history_arr)) {
      if (s->on_budget_event) {
        struct xAgentBudgetTruncateInfo ti;
        ti.entries_removed = keep_idx;
        s->on_budget_event((xAgentSession)s, xAgentBudgetEvent_Truncated, &ti,
                           s->budget_event_ud);
      }
    }

    /* The compact is done. The caller's on_done is NOT fired here
     * — they're still holding a Busy result. The next time they
     * call xAgentSessionInput, the budget gate will see the
     * (hopefully reduced) history and either let the turn through
     * or apply further policy. */
    return;
  }

  /* ── Normal (non-compact) on_done path ──────────────────────── */

  /* Private Query handle — needed for first_round_prompt_tokens. */
  struct xAgentQuery_ *q_priv = (struct xAgentQuery_ *)q;

  /* ── Calibrator update ──────────────────────────────────────────
   *
   * We compare the gate's pre-submit estimate (last_prompt_estimate)
   * against the FIRST round's provider-reported prompt_tokens. This
   * works for both single-round and multi-round tool-loop runs:
   *
   *   - The gate runs once per xAgentSessionInput, BEFORE the Query is
   *     submitted. Its estimate reflects the token count the provider
   *     will see on the FIRST round.
   *   - Subsequent tool-loop rounds add tool_results to the prompt,
   *     inflating later rounds' prompt_tokens beyond what the gate
   *     estimated. Using those later values would systematically
   *     inflate the factor.
   *   - The first round's prompt_tokens maps cleanly to the gate
   *     estimate in ALL cases — single-round text conversations AND
   *     multi-round tool loops alike. This eliminates the old
   *     "single_round" opt-out that left the calibrator permanently
   *     stuck at 1.0x / 0 samples for any session that used tools.
   *
   * Other opt-outs handled inside ai_budget_calibrator_update():
   *   - usage == NULL or first_round_prompt_tokens < 0 (unknown)
   *   - last_prompt_estimate == 0 (gate was Disabled or not run) */
  if (usage && q_priv->first_round_prompt_tokens >= 0 &&
      s->last_prompt_estimate > 0) {
    ai_budget_calibrator_update(&s->budget_calibrator, s->last_prompt_estimate,
                                q_priv->first_round_prompt_tokens);
  }
  /* Stash the first-round prompt_tokens for the next GatePassed
   * event — callers can display "estimated vs actual" without
   * the inflation that later tool-loop rounds would introduce.
   * We save this even when calibrator didn't update (e.g. gate
   * was Disabled) so the caller always sees the actual value
   * when available. */
  s->last_first_round_prompt_tokens = q_priv->first_round_prompt_tokens;
  /* Reset for the next run either way — a stale estimate crossing
   * query boundaries would be a foot-gun if some future code path
   * fires on_done without having gone through the gate. */
  s->last_prompt_estimate = 0;

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
  s->last_prompt_estimate           = 0;
  s->last_first_round_prompt_tokens = -1;

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
  s->history_arr = xArrayCreate(sizeof(struct xAgentSessionMsg_), 8, &kHistoryCbs);
  if (!s->history_arr) {
    free(s);
    return NULL;
  }

  return (xAgentSession)s;
}

xErrno xAgentSessionInput(xAgentSession sess, xAgentMessage msg) {
  if (!sess) return xErrno_InvalidArg;
  struct xAgentSession_ *s = (struct xAgentSession_ *)sess;

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
   * to this Session. xAgentQueryCreate also sets s->query so a second
   * Input call during the same run hits the Busy branch above. */
  struct xAgent_ *a  = (struct xAgent_ *)s->agent;
  xAgentQueryConf      qc = {0};
  qc.cbs               = SESSION_FWD_CBS;
  qc.cbs.user_data     = s;
  /* Only expose the confirmation gate if the host actually wired a
   * callback \u2014 otherwise leave on_tool_confirm NULL so needs_confirm
   * tools keep running without asking (backward-compatible default). */
  if (s->cbs.on_tool_confirm) {
    qc.cbs.on_tool_confirm = sess_fwd_on_tool_confirm;
  }
  qc.provider          = a->provider;
  qc.tools             = (const xAgentTool **)a->tools;
  qc.tools_count       = a->tools_count;
  qc.model             = s->model;
  qc.max_tokens        = s->max_tokens;
  qc.max_turns         = s->max_turns;
  qc.session           = sess;

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
   * on_finalizing so the consumer sees the data first. */
  if (s->on_l1_preserve) {
    xAgentSessionL1PreserveFunc hook  = s->on_l1_preserve;
    void                    *owner = s->l1_preserve_owner;
    s->on_l1_preserve              = NULL;
    s->l1_preserve_owner           = NULL;
    size_t hist_len                = xArrayLen(s->history_arr);
    /* Always invoke the hook on Finalizing so the owner can free
     * its context even when the history is empty.  Pass the
     * (possibly empty) array and its length; the callback is
     * responsible for handling n_msgs == 0 gracefully. */
    hook(sess, (const xAgentSessionMsg *)xArrayData(s->history_arr), hist_len,
         xAgentL1PreserveReason_Finalizing, owner);
  }

  if (s->on_finalizing) {
    xAgentSessionFinalizingFunc hook  = s->on_finalizing;
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

xAgentInputOrigin xAgentSessionOrigin(xAgentSession sess) {
  if (!sess) return xAgentInputOrigin_User;
  return ((struct xAgentSession_ *)sess)->origin;
}

const char *xAgentSessionId(xAgentSession sess) {
  if (!sess) return NULL;
  return ((struct xAgentSession_ *)sess)->session_id;
}
