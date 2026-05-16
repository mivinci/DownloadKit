/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * callbacks.cpp - xAgentSession callbacks (all output goes through
 *                 xline's above channel).
 */

#include "callbacks.h"

#include "ctx.h"
#include "output.h"
#include "repl.h"

#include <cstdio>
#include <cstdlib>

#include <x/agent/session.h>
#include <x/base/time.h>
#include <x/line/line.h>

void on_text(xAgentSession sess, const char *chunk, size_t len, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Close the thinking block (if any) before the visible reply
   * starts, so they don't visually merge AND the terminal doesn't
   * stay in faint mode for the rest of the output. */
  if (ctx->in_thinking) {
    end_thinking(ctx);
    ctx->saw_first_delta = true;
  } else if (!ctx->saw_first_delta) {
    /* No-op: xLinePrintAboveChunk naturally starts on a fresh row
     * above the prompt, so we don't need the leading '\n' the
     * blocking version used. */
    ctx->saw_first_delta = true;
  }
  /* Route through the active renderer pipeline. The vtable dispatches
   * to either xMdFeed (md) or above_chunk (raw) depending on which
   * renderer was selected at startup or via /md / /raw commands. */
  ctx->renderer.feed(ctx->renderer.state, chunk, len);
  ctx->reply_bytes += len;
}

/* Thinking stream: dim + prefix so it's obviously "model scratchpad"
 * and not the final answer. ANSI 2 = faint; most modern terminals
 * honour it (including macOS Terminal and iTerm2). On the rare
 * terminal that doesn't, the `[thinking]` prefix still telegraphs
 * intent. */
void on_thinking(xAgentSession sess, const char *chunk, size_t len, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  if (!ctx->in_thinking) {
    /* Open a new thinking block on its own line. */
    xLinePrintAboveChunk(ctx->line, "\x1b[2m[thinking] ");
    ctx->in_thinking = true;
  }
  above_chunk(ctx->line, chunk, len);
}

void on_tool(xAgentSession sess, const char *tool_name, int started,
             void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  above_printf(ctx->line, "\x1b[2m[tool] %s %s\x1b[0m",
               tool_name ? tool_name : "(null)",
               started ? "starting" : "finished");
}

void on_tool_output(xAgentSession sess, const char *tool_use_id,
                    const char *tool_name, const char *data, size_t len,
                    void *ud) {
  (void)sess;
  (void)tool_use_id;
  (void)tool_name;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Honor /verbose off: suppress tool data output.
   * Start/finish, command echo, and result summary still appear. */
  if (!ctx->verbose_tool_output) return;
  /* Close any open thinking block before showing tool output. */
  end_thinking(ctx);
  /* Render tool output faint; stream it as a chunk so multi-line
   * output doesn't force artificial breaks. The surrounding SGR
   * pair keeps the faint style scoped to the tool payload. */
  xLinePrintAboveChunk(ctx->line, "\x1b[2m");
  above_chunk(ctx->line, data, len);
  xLinePrintAboveChunk(ctx->line, "\x1b[0m");
}

void on_sidecar(xAgentSession sess, xAgentSidecarEvent event, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  switch (event) {
  case xAgentSidecarEvent_Started:
    above_printf(ctx->line, "\x1b[2m[sidecar] analyzing idle tool...\x1b[0m");
    break;
  case xAgentSidecarEvent_Done:
    above_printf(ctx->line, "\x1b[2m[sidecar] done\x1b[0m");
    break;
  }
}

static const char *done_reason_name(xAgentDoneReason r) {
  switch (r) {
  case xAgentDoneReason_Completed:
    return "completed";
  case xAgentDoneReason_MaxTurns:
    return "max_turns";
  case xAgentDoneReason_PromptTooLong:
    return "prompt_too_long";
  case xAgentDoneReason_Aborted:
    return "aborted";
  case xAgentDoneReason_ModelError:
    return "model_error";
  case xAgentDoneReason_ToolError:
    return "tool_error";
  case xAgentDoneReason_Stopped:
    return "stopped";
  }
  return "?";
}

void on_done(xAgentSession sess, xAgentDoneReason reason,
             const xAgentUsage *usage, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  /* Drain the active renderer before [done] chrome so any
   * still-pending delimiter bytes or open SGR spans are flushed
   * ahead of the faint block. Without this, an assistant reply
   * ending mid-bold would bleed bold into the [done] line. */
  ctx->renderer.flush(ctx->renderer.state);
  /* [done] is chrome — render the whole block faint so it recedes
   * and the model's answer above stays visually primary. Build the
   * full status line in a local buffer and emit it as one Above
   * call so the prompt repaints exactly once at the end. */
  char   line_buf[512];
  size_t off    = 0;
  double elapse = (xMonoMs() - ctx->input_ms) / 1000.0;
  off += std::snprintf(line_buf + off, sizeof(line_buf) - off,
                       "\x1b[2m\n[done] reason=%s reply_bytes=%zu elapse=%.2fs",
                       done_reason_name(reason), ctx->reply_bytes, elapse);
  /* Token accounting. prompt_tokens is the maximum across all
   * rounds (each round reports the full input size the provider
   * saw, so the last round's value is the total). completion_tokens
   * and total_tokens are additive per round. The provider fills -1
   * for fields it couldn't parse; we hide those so the line stays
   * clean for servers that only report a subset. A NULL usage means
   * the server never sent a usage object — rare in practice
   * (moonshot, openai, deepseek all support
   * stream_options.include_usage). */
  if (usage) {
    off += std::snprintf(line_buf + off, sizeof(line_buf) - off, " tokens=");
    if (usage->prompt_tokens >= 0) {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "%d",
                           usage->prompt_tokens);
    } else {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "?");
    }
    off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "/");
    if (usage->completion_tokens >= 0) {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "%d",
                           usage->completion_tokens);
    } else {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, "?");
    }
    if (usage->total_tokens >= 0) {
      ctx->total_tokens += usage->total_tokens;
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, " total=%d",
                           ctx->total_tokens);
    }
  }
  /* Context-budget snapshot. budget_remaining and budget_limit
   * are populated by the GatePassed event that fires at the start
   * of every round (before the Query is submitted). Displaying
   * remaining/limit gives the user a real-time view of how much
   * context headroom is left before the budget gate would trigger
   * TruncateTail or Summarize. A remaining of 0 means
   * the very next input is likely to hit the cap. est is the
   * pre-submit estimate for this round. */
  if (ctx->budget_limit > 0) {
    off += std::snprintf(line_buf + off, sizeof(line_buf) - off,
                         " budget=%zu/%zu est=%zu",
                         ctx->budget_remaining, ctx->budget_limit,
                         ctx->budget_estimated);
    if (ctx->last_actual_prompt >= 0) {
      off += std::snprintf(line_buf + off, sizeof(line_buf) - off, " actual=%d",
                           ctx->last_actual_prompt);
    }
  }
  std::snprintf(line_buf + off, sizeof(line_buf) - off, "\x1b[0m\n");
  xLinePrintAbove(ctx->line, line_buf);

  ctx->busy = false;
  /* Defensive: if the run tore down while a confirm was in flight
   * (Aborted / ModelError before the user answered), the session has
   * already invalidated those resolvers — Resolve() is a documented
   * no-op on a cancelled run. Drain our queue so the UI flips back
   * to normal instead of leaving a stale "confirm>" prompt. */
  if (ctx->confirm_active || !ctx->confirm_queue.empty()) {
    repl_drain_confirms_rejected(ctx, "run ended");
  }
  /* Natural completion of the user's run — nothing more to do.
   * The event loop keeps running so the editor stays interactive. */
}

void on_error(xAgentSession sess, xErrno err, const char *msg, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Same SGR hygiene as on_done — error might fire mid-thinking.
   * Errors are the one piece of chrome that should NOT recede — use
   * bold red (`\x1b[1;31m`) instead of faint so the user notices the
   * run failed at a glance. */
  end_thinking(ctx);
  /* Drop any renderer state: we're about to emit bold-red chrome
   * and don't want half-parsed bold/italic/code bleeding into it. */
  ctx->renderer.reset(ctx->renderer.state);
  above_printf(ctx->line, "\x1b[1;31m[error] errno=%d msg=%s\x1b[0m", (int)err,
               msg ? msg : "(none)");
  /* Surface the budget gate explicitly. PromptTooLong is the one
   * errno most likely to surprise a demo user ("I didn't do
   * anything weird, why did my innocuous follow-up get rejected?")
   * — it means either the rolling history plus the incoming
   * message overflowed sconf.budget.context_window with no room to
   * trim below keep_recent_turns, or the incoming message alone
   * is bigger than the cap. The fix is almost always "raise the
   * cap" for a calibrator demo; production callers would
   * typically switch to Summarize or a Callback policy. */
  if (err == xErrno_PromptTooLong) {
    above_printf(ctx->line, "\x1b[1;31m        hit budget cap — raise "
                            "sconf.budget.context_window\x1b[0m");
  }
  ctx->busy = false;
}

/* ── Budget-event callback ────────────────────────────────────────────
 *
 * Registered via sconf.budget.on_budget_event so the REPL user can
 * observe the Summarize lifecycle in real time. Events are
 * informational; ignoring them doesn't change session behaviour, but
 * surfacing them makes the budget demo much easier to follow — the
 * user sees why a subsequent xAgentSessionInput returned Busy
 * (Compacting) and knows when to retry (CompactDone). */
void on_budget_event(xAgentSession sess, xAgentBudgetEvent event,
                     const void *info, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);
  /* Budget events are chrome — render faint, same as [tool]/[done]. */
  switch (event) {
  case xAgentBudgetEvent_Compacting: {
    auto *ci = static_cast<const xAgentBudgetCompactInfo *>(info);
    above_printf(ctx->line,
                 "\x1b[2m[budget] compacting %zu old entries...\x1b[0m",
                 ci ? ci->entries_compacted : (size_t)0);
    break;
  }
  case xAgentBudgetEvent_CompactDone: {
    auto *cdi = static_cast<const xAgentBudgetCompactDoneInfo *>(info);
    if (cdi && cdi->summary_ok) {
      above_printf(ctx->line,
                   "\x1b[2m[budget] compact done — summary %zu tokens, "
                   "%zu entries affected\x1b[0m",
                   cdi->summary_tokens, cdi->entries_affected);
      /* Session auto-retries the pending message internally. */
    } else {
      /* Compact failed (empty summary / OOM / provider error). The
       * session left history untouched. Surface a clear error so the
       * user can decide: /clear, shorten input, or adjust
       * context_window. */
      above_printf(
          ctx->line,
          "\x1b[1;31m[error] compact failed — history unchanged. Try "
          "/clear or a shorter prompt.\x1b[0m");
    }
    break;
  }
  case xAgentBudgetEvent_GatePassed: {
    auto *gi = static_cast<const xAgentBudgetGateInfo *>(info);
    if (gi) {
      ctx->budget_limit       = gi->limit;
      ctx->budget_remaining   = gi->remaining;
      ctx->budget_estimated   = gi->estimated;
      ctx->last_actual_prompt = gi->last_first_round_prompt_tokens;
      above_printf(ctx->line,
                   "\x1b[2m[budget] gate passed — remaining %zu/%zu "
                   "tokens\x1b[0m",
                   gi->remaining, gi->limit);
    }
    break;
  }
  }
}
