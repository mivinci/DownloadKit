/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ctx.h - Shared REPL context for the xKit cli.
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

#ifndef XKIT_APPS_CLI_CTX_H
#define XKIT_APPS_CLI_CTX_H

#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/event.h>
#include <xline/line.h>

#include <deque>
#include <string>

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
  const char *hist_path   = nullptr; /* xline history file, for /history */

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
};

#endif /* XKIT_APPS_CLI_CTX_H */
