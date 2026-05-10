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
#include <xtui/md.h>

#include <deque>
#include <string>

struct CliModelConfig; /* apps/cli/config.h */

/* ── Renderer vtable ────────────────────────────────────────────────
 *
 * Thin C-style vtable that abstracts the output rendering pipeline.
 * Allows the REPL to switch between markdown→ANSI and raw pass-
 * through at runtime (e.g. via /md and /raw slash commands) without
 * scattering `if (md_enabled)` branches across every callback.
 *
 * Three operations mirror the xMd contract:
 *   feed  — ingest a byte chunk (may emit 0+ sink calls)
 *   flush — drain pending state, close open SGR spans
 *   reset — discard pending state + emit SGR reset (mid-stream cancel)
 *
 * Two built-in vtables are provided:
 *   g_renderer_md  — routes through xMd (markdown → ANSI)
 *   g_renderer_raw — routes through above_chunk (verbatim)
 *
 * The `state` pointer is passed as the first arg to every vfunc so
 * each backend can carry its own context (xMd* or ReplCtx*). */
struct Renderer {
  void (*feed)(void *state, const char *data, size_t len);
  void (*flush)(void *state);
  void (*reset)(void *state);
  void *state; /* opaque backend context */
};

/* Built-in renderer vtables (defined in output.cpp). */
extern const Renderer g_renderer_md;
extern const Renderer g_renderer_raw;

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
  /* Streaming markdown -> ANSI translator. Embedded by value -
   * the renderer is a tiny plain-data struct (~32 bytes, fixed
   * 4-byte pending buffer) so we keep it inline. xMdReset() is
   * called between runs so state doesn't bleed across turns. */
  xMd           md_renderer       = {};
  /* Active rendering pipeline. Switched at runtime by /md and /raw
   * commands. The vtable routes feed/flush/reset to either the
   * md_renderer above or the raw above_chunk path. Initialised in
   * main.cpp based on isatty(stdout) / $TERM. */
  Renderer      renderer          = {};
  const char   *renderer_name     = nullptr; /* "md" or "raw" */
  size_t        budget_limit     = 0;   /* from last GatePassed event */
  size_t        budget_remaining = 0;   /* from last GatePassed event */
  size_t        budget_estimated = 0;   /* pre-submit estimate (tokens) */
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
   * a successful switch to pull the selected entry's budget block
   * and push it into the session's budget gate so the thresholds
   * track the active model. Owned by main (CliModelConfig lives
   * for the whole process); ReplCtx must never free it. */
  const CliModelConfig *model_cfg = nullptr;

  /* Session-wide default budget thresholds: what the session was
   * configured with at create time, AFTER cascading
   * built-in-defaults <- top-level "budget" <- (chosen model's
   * "budget"). Mirrors sconf.budget at startup so /model switches
   * back to entries without per-model overrides don't silently
   * inherit the previous (possibly much larger) model's window or
   * trim threshold. Threshold fields only — policy and the event
   * callbacks are owned by main and replayed separately. */
  xAgentBudgetConf default_budget{};

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

/* Convenience: switch ctx->renderer to the md or raw backend.
 * Must be declared after ReplCtx is complete. */
void renderer_use_md(ReplCtx *ctx);
void renderer_use_raw(ReplCtx *ctx);

#endif /* MOO_APPS_CLI_CTX_H */
