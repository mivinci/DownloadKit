/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ctx.h - Shared REPL context for the moo cli.
 *
 * The cli is split across several translation units (output, slash,
 * callbacks, repl, main) that all need to reach into the same REPL
 * state. Rather than threading a dozen parameters through each call
 * we pass a pointer to `ReplCtx` as the `void *ud` for every xline /
 * xagent callback, and as the arg to the top-level REPL glue.
 *
 * This header is the single anchor for those shared types. It has
 * no implementation file — it's types only. Everything else lives
 * next to whichever TU owns the behaviour.
 */

#ifndef MOO_APPS_CLI_CTX_H
#define MOO_APPS_CLI_CTX_H

#include <xagent/model.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/event.h>
#include <xline/line.h>

#include <deque>
#include <string>

struct CliModelConfig; /* apps/cli/config.h */

/* One queued tool-confirm request. See the "Tool-confirm gate" block
 * in repl.cpp for the full semantics. */
struct PendingConfirm {
  std::string               tool_name;
  std::string               tool_use_id;
  std::string               args_json;
  xAgentToolConfirmResolver resolver;
};

struct ReplCtx {
  xEventLoop    loop             = nullptr;
  xAgentSession sess             = nullptr;
  xLineHandle   line             = nullptr; /* current async editor */
  xEventSource  src              = nullptr; /* loop fd registration */
  bool          busy             = false;   /* AI run in flight */
  bool          pending_retry    = false; /* retry pending_text after compact */
  char         *pending_text     = nullptr; /* stashed submit text, owned */
  bool          saw_first_delta  = false;
  bool          in_thinking      = false; /* currently streaming thinking? */
  size_t        reply_bytes      = 0;
  int           total_tokens     = 0;   /* cumulative across all rounds */
  size_t        budget_limit     = 0;   /* from last GatePassed event */
  size_t        budget_remaining = 0;   /* from last GatePassed event */
  double        budget_factor    = 1.0; /* EWMA calibrator factor */
  size_t        budget_samples   = 0;   /* calibrator observation count */
  size_t        budget_estimated = 0;   /* calibrated pre-submit estimate */
  int last_actual_prompt = -1; /* provider-reported first-round prompt_tokens */
  uint64_t    input_ms   = 0;  /* monotonic timestamp (ms) at user input */
  bool        should_exit = false;   /* set by /exit handler */

  /* ── Model registry + current selection ────────────────────────
   * The registry is borrowed from the CliModelConfig owned by main;
   * it outlives the ReplCtx. current_model_id tracks the id the
   * session was last switched to via xAgentSessionSetModel (or the
   * initial "default" id loaded from models.json). Used by /model
   * to display and switch, and by the banner to show the active
   * backend. */
  xAgentModelRegistry model_registry  = nullptr;
  std::string         current_model_id;

  /* Borrowed pointer to the parsed models.json. Used by /model on
   * a successful switch to pull the selected entry's context_window
   * and push it into the session's budget gate so the token ceiling
   * tracks the active model. Owned by main (CliModelConfig lives
   * for the whole process); ReplCtx must never free it. */
  const CliModelConfig *model_cfg = nullptr;

  /* Session-wide default context window (in tokens) applied whenever
   * the selected model entry carries no explicit "context_window".
   * Mirrors sconf.budget.max_tokens at startup so /model switches to
   * entries without a per-model override don't silently inherit the
   * previous (possibly much larger) model's window. */
  size_t default_context_window = 0;

  /* Tool-confirm gate ────────────────────────────────────────────────
   * When a needs_confirm tool (currently just shell) is about to run,
   * on_tool_confirm pushes a PendingConfirm onto this queue and flips
   * the REPL into "confirm mode": the editor is reopened with a
   * dedicated prompt and a below-panel showing the pending command,
   * and the next line the user submits is parsed as a decision rather
   * than sent to the model. Queued (not single-slot) so that a single
   * assistant turn can chain multiple tool calls — the user sees them
   * one at a time in arrival order. */
  std::deque<PendingConfirm> confirm_queue;
  bool                       confirm_active = false;

  /* Bypass gate ────────────────────────────────────────────────────
   * When true, on_tool_confirm short-circuits: every needs_confirm
   * tool is auto-allowed without prompting. Toggled by /bypass.
   * The flag is process-local and resets to false on each cli
   * invocation — persisting it across runs would be too easy to
   * forget about, and the point of the flag is that you explicitly
   * opt in for the current session. */
  bool bypass_confirm = false;
};

#endif /* MOO_APPS_CLI_CTX_H */
