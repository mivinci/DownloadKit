/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * repl.cpp - Async REPL glue.
 *
 * The editor is kept alive for the whole REPL lifetime: users may
 * still edit and execute slash commands (notably /cancel) while the
 * AI run is in flight. When the user presses Enter we briefly close
 * the session so the line dispatch code can run without fighting the
 * edit region, then reopen a fresh session right after — this mirrors
 * xline_async.c's pattern. AI callbacks (on_text, on_tool, on_done,
 * …) write through xLinePrintAbove/Chunk so the prompt row stays
 * intact regardless of what the model is doing.
 */

#include "repl.h"

#include "output.h"
#include "slash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <xagent/message.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/event.h>
#include <xbase/time.h>
#include <xline/line.h>

static void repl_line_cb(int fd, xEventMask mask, void *arg);

/* Forward decls for the confirm-gate machinery. Bodies live further
 * down near the rest of the REPL glue. */
static void repl_update_confirm_panel(ReplCtx *ctx);
static void repl_enter_confirm_mode(ReplCtx *ctx);
static void repl_leave_confirm_mode(ReplCtx *ctx);
static int  repl_refresh_confirm_editor(ReplCtx *ctx);

int repl_open_line_with_prompt(ReplCtx *ctx, const char *prompt) {
  ctx->line = xLineBegin(prompt ? prompt : "");
  if (!ctx->line) {
    std::fprintf(stderr,
                 "xLineBegin failed (dumb tty? another session live?)\n");
    return -1;
  }
  int fd = xLineFd(ctx->line);
  if (fd < 0) {
    std::fprintf(stderr,
                 "xLineFd returned %d — not pollable on this platform\n", fd);
    xLineEnd(ctx->line);
    ctx->line = nullptr;
    return -1;
  }
  ctx->src = xEventAdd(ctx->loop, fd, xEvent_Read, repl_line_cb, ctx);
  if (!ctx->src) {
    std::fprintf(stderr, "xEventAdd failed for tty fd=%d\n", fd);
    xLineEnd(ctx->line);
    ctx->line = nullptr;
    return -1;
  }
  return 0;
}

int repl_open_line(ReplCtx *ctx) {
  return repl_open_line_with_prompt(ctx, "");
}

void repl_close_line(ReplCtx *ctx) {
  if (ctx->src) {
    xEventDel(ctx->loop, ctx->src);
    ctx->src = nullptr;
  }
  if (ctx->line) {
    xLineEnd(ctx->line);
    ctx->line = nullptr;
  }
}

/* ── Tool-confirm gate ──────────────────────────────────────────────
 *
 * The shell tool is created with needs_confirm=1 in xAgentToolShellCreate,
 * so every proposed shell invocation drives on_tool_confirm here before
 * the handler is allowed to run. We surface the request by reopening
 * the line editor under a dedicated prompt ("confirm> ") with a below-
 * panel that renders the tool name, the raw args JSON (short enough
 * for shell — {"command":"..."} — that a JSON parser isn't worth
 * pulling in for a demo), and the key hints. The next line the user
 * submits is routed to repl_handle_confirm_line instead of the model.
 *
 * Multiple confirms that arrive in the same assistant turn (the model
 * chained two shell calls) queue up behind the active one; the user
 * sees them one at a time, in arrival order, until the queue drains
 * and the REPL flips back to normal chat mode.
 *
 * Resolver lifetime: the session guarantees the resolver handle is
 * valid from on_tool_confirm until xAgentToolConfirmResolve is called
 * exactly once (or the owning run is cancelled, in which case Resolve
 * becomes a silent no-op — we still call it on /cancel paths to keep
 * our own bookkeeping clean).
 */

/* Build the body of the confirm panel for the head-of-queue item.
 * Kept as a helper so both enter_confirm_mode and dequeuing the next
 * request can refresh the panel without duplicating format strings. */
static void repl_update_confirm_panel(ReplCtx *ctx) {
  if (ctx->confirm_queue.empty()) return;
  const PendingConfirm &pc = ctx->confirm_queue.front();
  std::string           body;
  body.append("tool: ");
  body.append(pc.tool_name);
  body.append("\nargs: ");
  body.append(pc.args_json);
  if (ctx->confirm_queue.size() > 1) {
    char qtail[64];
    std::snprintf(qtail, sizeof(qtail), "\n(+%zu more queued)",
                  ctx->confirm_queue.size() - 1);
    body.append(qtail);
  }
  body.append("\n\n  [y] allow   [n] reject   [r <reason>] reject w/ reason");
  xLineSetBelowPanel(ctx->line, "", body.c_str());
}

/* Enter confirm mode. Caller must have enqueued at least one
 * PendingConfirm first. Safe to call while the editor is open — we
 * close + reopen with the new prompt so the visual transition is
 * clean (old prompt's cursor column is dropped). */
static void repl_enter_confirm_mode(ReplCtx *ctx) {
  if (ctx->confirm_queue.empty()) return;
  repl_close_line(ctx);
  /* Suppress the default "> " prompt marker while we're asking for a
   * tool-use decision — otherwise it renders immediately after our
   * "confirm> " prefix and looks like a stray glyph. Restored in
   * repl_leave_confirm_mode. */
  xLineSetPromptMarker("", NULL);
  if (repl_open_line_with_prompt(ctx, "\x1b[1;33mconfirm>\x1b[0m ") != 0) {
    /* If we can't reopen the editor we can't ask the user — fall
     * back to Reject so the run doesn't hang forever. */
    xLineSetPromptMarker(NULL, NULL);
    while (!ctx->confirm_queue.empty()) {
      xAgentToolConfirmResolve(ctx->confirm_queue.front().resolver,
                               xAgentToolDecision_Reject,
                               "tty unavailable for confirm");
      ctx->confirm_queue.pop_front();
    }
    ctx->confirm_active = false;
    xEventLoopStop(ctx->loop);
    return;
  }
  ctx->confirm_active = true;
  above_printf(ctx->line,
               "\x1b[1;33m[confirm] tool '%s' wants to run — approve?\x1b[0m",
               ctx->confirm_queue.front().tool_name.c_str());
  repl_update_confirm_panel(ctx);
}

static void repl_leave_confirm_mode(ReplCtx *ctx) {
  repl_close_line(ctx);
  /* Restore the default "> " marker before reopening the main chat
   * prompt so normal input lines look the way the user expects. */
  xLineSetPromptMarker(NULL, NULL);
  if (repl_open_line(ctx) != 0) {
    xEventLoopStop(ctx->loop);
    return;
  }
  ctx->confirm_active = false;
  /* No xLineClearBelowPanel here: the below panel lived on the old
   * handle released by repl_close_line above. The fresh handle from
   * repl_open_line starts with empty panel buffers, so clearing would
   * only trigger a redundant wipe+repaint cycle. */
}

/* Tear down the just-submitted "confirm>" editor session and open a
 * fresh one under the same prompt, so we can continue reading user
 * input while staying in confirm mode.
 *
 * Why this exists: xline handles are single-shot. Once xLineStep
 * returns XLINE_STEP_LINE and xLineTake has consumed the buffer,
 * the session is terminal — subsequent xLineStep calls won't accept
 * new keystrokes and the editor is effectively dead. Chat flow
 * handles this with close+open around each submitted line; every
 * confirm-mode path that wants to keep reading (another item
 * queued behind the one we just decided, empty-input redraw,
 * unrecognised-input redraw) needs the same dance. We factor it
 * out here so the three call sites stay in sync. The caller is
 * expected to set up the below panel afterwards via
 * repl_update_confirm_panel. Returns 0 on success; on failure
 * it's already stopped the event loop. */
static int repl_refresh_confirm_editor(ReplCtx *ctx) {
  repl_close_line(ctx);
  /* Marker was already cleared when we first entered confirm mode;
   * no need to touch it again. Just reopen under the same "confirm>"
   * prompt so the visual transition is invisible to the user. */
  if (repl_open_line_with_prompt(ctx, "\x1b[1;33mconfirm>\x1b[0m ") != 0) {
    xEventLoopStop(ctx->loop);
    return -1;
  }
  ctx->confirm_active = true;
  return 0;
}

/* Reject everything in the confirm queue and leave confirm mode. Used
 * on cancel / interrupt paths so the session doesn't stall waiting for
 * a resolver the user has abandoned. Resolve() on a resolver whose
 * owning run was already cancelled is a documented silent no-op, so
 * we can call it unconditionally without racing the session's own
 * teardown. */
void repl_drain_confirms_rejected(ReplCtx *ctx, const char *reason) {
  while (!ctx->confirm_queue.empty()) {
    xAgentToolConfirmResolve(ctx->confirm_queue.front().resolver,
                             xAgentToolDecision_Reject,
                             reason ? reason : "cancelled");
    ctx->confirm_queue.pop_front();
  }
  if (ctx->confirm_active) repl_leave_confirm_mode(ctx);
}

/* Handle one line submitted while confirm mode is active. The line
 * has already been xLineTake()'d by the caller; we own it only for
 * the duration of the call (caller frees). Semantics:
 *
 *   y / yes / a / allow  → Allow
 *   n / N                → Reject with default reason
 *   r <reason>           → Reject with user-supplied reason
 *   empty / unrecognised → no-op; redraw panel, stay in confirm mode
 *
 * Empty input is deliberately *not* a shortcut for Reject. Confirm
 * gates on shell commands are destructive-adjacent and silently
 * consuming stray Enters (e.g. leaked from the previous chat editor
 * just before on_tool_confirm swapped us in) has bitten users. The
 * redraw-and-wait path makes the decision strictly opt-in: the user
 * must type one of y/n/r explicitly. repl_enter_confirm_mode also
 * tcflush()es the kernel tty input queue on entry as a first line
 * of defence; this branch is the belt-and-braces second line. */
static void repl_handle_confirm_line(ReplCtx *ctx, const char *raw) {
  if (ctx->confirm_queue.empty()) {
    /* Shouldn't happen: the gate shouldn't be active with an empty
     * queue. Recover defensively. */
    repl_leave_confirm_mode(ctx);
    return;
  }
  PendingConfirm head = std::move(ctx->confirm_queue.front());
  ctx->confirm_queue.pop_front();

  /* Normalize: skip leading whitespace. */
  const char *s = raw ? raw : "";
  while (*s == ' ' || *s == '\t')
    ++s;

  xAgentToolDecision decision = xAgentToolDecision_Reject;
  const char        *reason   = "user declined execution";

  if (*s == '\0') {
    /* Empty submission — almost never intentional at a confirm gate.
     * Push the request back and redraw. A faint hint nudges the
     * user toward an explicit answer without shouting like the
     * "unrecognised" warning would. The editor session just
     * submitted an (empty) line and is therefore terminal; we must
     * close+reopen it before we can read the next keystroke. */
    ctx->confirm_queue.push_front(std::move(head));
    if (repl_refresh_confirm_editor(ctx) != 0) return;
    above_printf(ctx->line,
                 "\x1b[2m(type y to allow, n to reject, r <reason> "
                 "to reject with reason)\x1b[0m");
    repl_update_confirm_panel(ctx);
    return;
  } else if (*s == 'n' || *s == 'N') {
    decision = xAgentToolDecision_Reject;
    reason   = "user declined execution";
  } else if (*s == 'y' || *s == 'Y' || *s == 'a' || *s == 'A') {
    decision = xAgentToolDecision_Allow;
    reason   = nullptr;
  } else if (*s == 'r' || *s == 'R') {
    decision = xAgentToolDecision_Reject;
    /* Skip the leading 'r' and any whitespace — the rest is the
     * user's reason. Fallback to default if they typed "r" alone. */
    const char *tail = s + 1;
    while (*tail == ' ' || *tail == '\t')
      ++tail;
    reason = *tail ? tail : "user declined execution";
  } else {
    /* Unrecognised — repaint the panel, re-read. Push back onto
     * head of queue and stay in confirm mode. Same single-shot
     * caveat as the empty-input branch: refresh the editor before
     * we print/repaint so the new session owns the below panel. */
    ctx->confirm_queue.push_front(std::move(head));
    if (repl_refresh_confirm_editor(ctx) != 0) return;
    above_printf(ctx->line,
                 "\x1b[33m(unrecognised; use y / n / r <reason>)\x1b[0m");
    repl_update_confirm_panel(ctx);
    return;
  }

  /* Echo the decision so the transcript records what the user
   * chose — useful for debugging a run that ended with ToolError.
   * We include the raw input verbatim because xline clears the
   * submitted "confirm> …" line on done, so without this echo the
   * history would show the prompt but not the keystroke.
   *
   * Lifecycle note: when this is the last pending confirm and the
   * AI run is still in flight, we fall back to the regular chat
   * editor immediately rather than keeping "confirm>" up as an
   * idle placeholder. The busy guard in repl_handle_line catches
   * any accidental chat submits during the HTTP gap between tool
   * rounds and nudges the user with an "AI is busy" notice +
   * Up-arrow recovery, so leaving the user stranded at a fake
   * confirm prompt just to absorb keystrokes isn't worth the
   * extra state-machine surface area. */
  const char *echo = (raw && *raw) ? raw : "(empty)";

  /* Refresh the xline session BEFORE emitting the decision echo or
   * calling Resolve. Why: the editor that just produced this line
   * is in state DONE_LINE, and xLinePrintAbove / xLinePrintAboveChunk
   * silently drop writes unless the session is in INIT or RUNNING
   * (see the state guard at the top of xLinePrintAbove in async.c).
   *
   * Without refreshing first, two classes of output vanish:
   *   1. our own "[confirm] … → allowed/rejected (…)" echo right
   *      after this comment, which the user relies on to confirm
   *      their keystroke was registered;
   *   2. everything the synchronous Resolve call graph emits —
   *      on_tool(started=1) ("[tool] shell starting"), and for
   *      fast synchronous tools the on_tool_output stream plus
   *      on_tool(started=0) ("[tool] shell finished") too.
   *
   * Refreshing first puts us back on a RUNNING session so every
   * above_printf/above_chunk from here through the end of Resolve
   * lands on screen. We stay in confirm mode for the refresh: the
   * same "confirm>" prompt is still the right visual state whether
   * the next tool_use arrives synchronously (pile-on) or not
   * (queue empties and we leave_confirm_mode below). */
  if (repl_refresh_confirm_editor(ctx) != 0) return;

  if (decision == xAgentToolDecision_Allow) {
    above_printf(ctx->line, "\x1b[2m[confirm] %s → allowed (%s)\x1b[0m",
                 head.tool_name.c_str(), echo);
  } else {
    above_printf(ctx->line, "\x1b[2m[confirm] %s → rejected (%s): %s\x1b[0m",
                 head.tool_name.c_str(), echo,
                 reason ? reason : "(none)");
  }

  /* Resolve is synchronous and drives the tool handler's on_tool /
   * on_command / on_tool_output callbacks on this stack frame.
   * Those all stream through above_printf / above_chunk on the
   * freshly-refreshed session, which correctly route around the
   * "confirm>" editor's edit region and below panel. */
  xAgentToolConfirmResolve(head.resolver, decision, reason);

  if (!ctx->confirm_queue.empty()) {
    /* Another request is already queued (the model chained two
     * tool_use blocks in the same round, or — more commonly —
     * Resolve's synchronous tool-handler path re-entered
     * on_tool_confirm for the next tool_use in the same round).
     * The editor we refreshed above Resolve is still live (the
     * pile-on branch of on_tool_confirm only repaints the below
     * panel), so we just need to refresh the decision panel to
     * show the new head and nudge the user with a banner. */
    repl_update_confirm_panel(ctx);
    above_printf(
      ctx->line,
      "\x1b[1;33m[confirm] next: tool '%s' wants to run — approve?\x1b[0m",
      ctx->confirm_queue.front().tool_name.c_str());
    return;
  }

  /* Queue drained. Flip back to the regular chat editor right
   * away, regardless of ctx->busy. If the AI is still running
   * (typical when we just Allowed a tool — the next submit_round
   * is racing out over HTTP), any chat input the user types in
   * the gap is caught by repl_handle_line's `if (ctx->busy)`
   * guard, echoed into history, and the user gets a faint
   * "(AI is busy — use /cancel to interrupt, then resend with
   * Up-arrow)" hint. That's a better UX than staring at a
   * "confirm>" prompt with an idle-hint panel between tool
   * rounds; visual chrome reflects state truthfully. */
  repl_leave_confirm_mode(ctx);
}

/* Session callback: a needs_confirm tool wants to run. */
void on_tool_confirm(xAgentSession sess, const char *tool_name,
                     const char *tool_use_id, const char *args_json,
                     xAgentToolConfirmResolver resolver, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  end_thinking(ctx);

  /* Bypass mode short-circuit. We intentionally don't push onto the
   * confirm queue or touch confirm_active — the point of bypass is
   * zero user interaction, so skipping the gate entirely keeps the
   * UI free of the "confirm>" prompt and below-panel flicker. We
   * still log a single faint line so bypass-enabled runs don't hide
   * the fact that a confirm-worthy tool just ran (searchable in
   * history, visible while scrolling). The resolver handle is
   * documented as valid from the callback until Resolve is called,
   * so calling it synchronously here is safe. */
  if (ctx->bypass_confirm) {
    above_printf(ctx->line,
                 "\x1b[2m[bypass] %s \u2192 auto-allow\x1b[0m",
                 tool_name ? tool_name : "(null)");
    xAgentToolConfirmResolve(resolver, xAgentToolDecision_Allow, nullptr);
    return;
  }

  PendingConfirm pc;
  pc.tool_name   = tool_name ? tool_name : "(null)";
  pc.tool_use_id = tool_use_id ? tool_use_id : "";
  pc.args_json   = args_json ? args_json : "";
  pc.resolver    = resolver;
  ctx->confirm_queue.push_back(std::move(pc));

  /* Already in confirm mode? Then this is a pile-on: the previous
   * head was just resolved and the synchronous tool-loop re-entered
   * us for the next tool_use in the same round, *or* the previous
   * head's repl_handle_confirm_line enqueued us before it got to
   * its own "next queued" branch. Either way the "confirm>" editor
   * is already live and attached to a fresh xline session — just
   * repaint the below panel to show the new head (and an accurate
   * "+N queued" count if there's more lined up behind us). No
   * banner: the user is mid-decision on the current head and a
   * new banner here would just be noise. */
  if (ctx->confirm_active) {
    repl_update_confirm_panel(ctx);
    return;
  }
  repl_enter_confirm_mode(ctx);
}

/* Submit a user message to the session. Handles the Busy → compact
 * → retry dance asynchronously: if the gate returns Busy we stash
 * the text and wait for on_budget_event(CompactDone) to resubmit.
 * xAgentMessageFromText returns a thread-local borrow-view backed by
 * caller-owned storage, so we must keep the text alive until the
 * session has actually accepted (and internally duplicated) it —
 * which, on the Ok path, happens before xAgentSessionInput returns. */
xErrno repl_submit_text(ReplCtx *ctx, const char *text) {
  /* Degraded "no model configured" mode: models.json was missing at
   * startup so main.cpp never built the agent and ctx->sess is
   * nullptr. Reject the submit with an actionable hint instead of
   * crashing on xAgentSessionInput. The user can still reach
   * /help, /exit, etc.; creating the file and relaunching is the
   * documented path to enable chat. We don't try to hot-reload
   * models.json from here because every downstream object (agent,
   * session, tool handlers) was sized against the config at boot. */
  if (!ctx->sess) {
    above_printf(ctx->line,
                 "\x1b[1;33m[no model]\x1b[22;39m chat is disabled \u2014 "
                 "edit models.json in your data_dir and restart "
                 "(see /help).");
    return xErrno_InvalidArg;
  }

  ctx->saw_first_delta = false;
  ctx->in_thinking     = false;
  ctx->reply_bytes     = 0;
  ctx->input_ms        = xMonoMs();
  /* Drop any residual renderer state from a previous run (shouldn't
   * happen after a clean on_done, but defensive against aborted
   * runs where Flush() may not have fired). */
  ctx->renderer.reset(ctx->renderer.state);

  xAgentMessage m   = xAgentMessageFromText(text);
  xErrno        err = xAgentSessionInput(ctx->sess, m);
  if (err == xErrno_Busy) {
    /* A budget compact is in flight. Stash a copy of the text
     * (the caller's buffer may be freed before CompactDone fires)
     * and let on_budget_event re-enter submit on our behalf. */
    if (ctx->pending_text) std::free(ctx->pending_text);
    ctx->pending_text = strdup(text);
    if (!ctx->pending_text) {
      /* OOM: fail loudly rather than silently swallow the user's
       * message. The retry flag stays false so on_budget_event's
       * CompactDone branch doesn't fire a NULL resubmit. */
      ctx->pending_retry = false;
      above_printf(ctx->line,
                   "\x1b[1;31m[error] out of memory stashing pending text "
                   "\u2014 please resend after compact completes\x1b[0m");
      return xErrno_NoMemory;
    }
    ctx->pending_retry = true;
    above_printf(
      ctx->line,
      "\x1b[2m(session busy \u2014 will resubmit after compact)\x1b[0m");
    return xErrno_Busy;
  }
  if (err != xErrno_Ok) {
    above_printf(ctx->line,
                 "\x1b[1;31m[error] input rejected (errno=%d)\x1b[0m",
                 (int)err);
    if (err == xErrno_PromptTooLong) {
      above_printf(ctx->line, "\x1b[1;31m        hit budget cap — raise "
                              "sconf.budget.context_window or lower "
                              "keep_recent_turns\x1b[0m");
    }
    return err;
  }
  ctx->busy = true;
  return xErrno_Ok;
}

/* Decide what to do with a completed line. Slash commands are
 * intercepted locally; chat input is submitted to the session iff
 * no run is currently active. Returning non-zero asks the caller
 * to stop the REPL. */
static int repl_handle_line(ReplCtx *ctx, char *line) {
  if (!line) return 0;
  size_t len = std::strlen(line);
  if (len == 0) return 0;

  if (line[0] == '/') {
    /* Keep the command in the in-memory history so Up-arrow and
     * /history can still reach it, but mark it transient so it
     * never reaches the on-disk session history file. The submit
     * echo ("> /help") was already emitted upstream in
     * repl_line_cb while the editor was live. */
    xLineHistoryMarkLastTransient();
    slash_dispatch(ctx, line);
    return ctx->should_exit ? 1 : 0;
  }

  if (ctx->busy) {
    /* The AI is still working; reject the submit but keep the
     * entry in history so the user can Up-arrow and resend once
     * /cancel (or on_done) clears the flag. */
    above_printf(ctx->line,
                 "\x1b[33m(AI is busy \u2014 use /cancel to interrupt, then "
                 "resend with Up-arrow)\x1b[0m");
    return 0;
  }

  /* Real chat submit. The stale slash-command panel (if any) and
   * the submit-line echo ("> ...") were both handled upstream in
   * repl_line_cb while the previous editor session was still live;
   * by the time we get here the old session is already torn down
   * and a fresh one is up, so there's nothing left to clean. */
  (void)repl_submit_text(ctx, line);
  return 0;
}

static void repl_line_cb(int fd, xEventMask mask, void *arg) {
  (void)fd;
  (void)mask;
  ReplCtx *ctx = static_cast<ReplCtx *>(arg);
  if (!ctx->line) return;

  for (;;) {
    xLineStepResult r = xLineStep(ctx->line);
    switch (r) {
    case XLINE_STEP_PENDING:
      return;
    case XLINE_STEP_LINE: {
      char *s = xLineTake(ctx->line);
      /* Confirm-gate branch: the active editor is the "confirm> "
       * one, so the line is a decision, not chat. Handle it in-
       * place (which may reopen the line itself when the queue
       * drains) and bail out — don't fall through to the normal
       * open/close dance the chat path uses. */
      if (ctx->confirm_active) {
        xLineHistoryRemoveLast(); /* don't pollute chat history */
        repl_handle_confirm_line(ctx, s);
        xLineFree(s);
        return;
      }
      /* Retire any below panel left over from a previous /help etc.
       * *before* we close the session. The below panel occupies real
       * rows between the prompt and the bottom of the viewport; when
       * it stays up at submit time, xLineEnd's edit-region teardown
       * leaves the cursor above the panel but the terminal still has
       * those panel rows on screen until we emit output that rolls
       * them out. Clearing here, while the session is live, tears
       * the panel down cleanly and frees the row budget before close.
       *
       * Slash commands that want a panel (e.g. /help) re-install it
       * on the new session after dispatch; clearing here is a no-op
       * for them beyond collapsing the old panel's screen footprint
       * first, which is what we want. */
      xLineClearBelowPanel(ctx->line);
      /* Close the finished editor session, open a fresh one, then
       * echo the user's submitted line on the new session via
       * above_printf.
       *
       * Why *this* order (close → open → echo on new session):
       *   - Echoing via above_printf on the *old* session worked for
       *     chat paths but confuses the first SetBelowPanel issued
       *     on the *new* session: the new session's start_row (CPR
       *     result) lands one row below the echo, and if the panel
       *     is tall enough to overflow the viewport, xline's anchor
       *     fast-paths into anchor_stuck and lets the terminal
       *     auto-scroll. The rows that get scrolled into scrollback
       *     are the ones above the new start_row — i.e. the echo
       *     we just emitted. User's submitted line vanishes the
       *     moment a tall panel (e.g. the tool-confirm body) comes
       *     up later in the turn.
       *   - Echoing in cooked mode between End and Begin has the
       *     same symptom in the opposite direction: the printf-ed
       *     row ends up at `CPR-measured start_row - 1`, outside
       *     xline's managed region. Any later scroll triggered by
       *     panel overflow in the new session yanks it into
       *     scrollback.
       *   - Echoing via above_printf on the *new* session makes the
       *     echo part of xline's own above-region stream. The
       *     anchor/wipe math treats it as a flushed row (just like
       *     streaming model output), which means subsequent anchor
       *     re-computations correctly include the echo when sizing
       *     the edit region, and the panel-row prediction added to
       *     xline_anchor_edit_bottom keeps the whole edit region on
       *     screen. When the terminal does have to scroll (because
       *     the above region has genuinely grown past one screen),
       *     the echo rolls into scrollback in the same chronological
       *     order as the rest of the transcript — no special-case
       *     disappearance. */
      repl_close_line(ctx);
      if (repl_open_line(ctx) != 0) {
        xLineFree(s);
        xEventLoopStop(ctx->loop);
        return;
      }
      if (s && s[0] != '\0') {
        above_printf(ctx->line, "\x1b[2m>\x1b[22m %s", s);
      }
      int want_stop = repl_handle_line(ctx, s);
      xLineFree(s);
      if (want_stop) {
        xEventLoopStop(ctx->loop);
        return;
      }
      return;
    }
    case XLINE_STEP_INTERRUPT: {
      /* User hit Ctrl-C. xline disables ISIG in raw mode, so ^C
       * never becomes a real SIGINT — it surfaces here instead.
       *
       * Two distinct semantics depending on AI state:
       *   busy → abort the in-flight run, keep the REPL alive
       *          (mirrors /cancel exactly: xAgentSessionCancel is
       *          async, on_done will eventually arrive with
       *          reason=Aborted and flip ctx->busy back off).
       *   idle → treat as a request to leave the REPL. A second
       *          ^C on an empty prompt is the conventional exit.
       *
       * Either way the old editor session has already been
       * finalised by xline (buffer wiped, state = DONE_INTERRUPT),
       * so we must close + reopen it to get a fresh prompt drawn
       * with the cursor back at column 0. */
      repl_close_line(ctx);
      if (ctx->busy || ctx->confirm_active) {
        if (repl_open_line(ctx) != 0) {
          xEventLoopStop(ctx->loop);
          return;
        }
        above_printf(ctx->line, "\x1b[2m[cancel] aborting run…\x1b[0m");
        if (ctx->confirm_active) {
          /* Drain pending resolvers first so the session's tool-loop
           * can unwind immediately instead of blocking on a user
           * decision that will never come. */
          repl_drain_confirms_rejected(ctx, "cancelled by user");
        }
        xAgentSessionCancel(ctx->sess);
        return;
      }
      xEventLoopStop(ctx->loop);
      return;
    }
    case XLINE_STEP_EOF:
    case XLINE_STEP_ERROR:
      /* Ctrl-D on empty input, or a fatal tty error. Treat either
       * as a clean shutdown. */
      repl_close_line(ctx);
      xEventLoopStop(ctx->loop);
      return;
    }
  }
}

void repl_on_sigint(int signo, void *arg) {
  /* Kept as a no-op stub only so grep/history still maps ^C to a
   * single place. In practice we never get here: xline puts the
   * tty in raw mode with ISIG cleared, so Ctrl-C arrives as a
   * byte (0x03) that xLineStep surfaces as XLINE_STEP_INTERRUPT.
   * The real cancel/exit logic lives in repl_on_readable's
   * INTERRUPT arm. This watch is only retained for the rare
   * paths that can still deliver SIGINT (kill -INT, GUI terminal
   * sending SIGINT explicitly): redirect them to the same
   * behaviour as the in-band interrupt. */
  (void)signo;
  ReplCtx *ctx = static_cast<ReplCtx *>(arg);
  if (ctx->busy) {
    xAgentSessionCancel(ctx->sess);
    return;
  }
  /* Idle ^C from out-of-band SIGINT (kill -INT, IDE stop button,
   * etc.): tear down the event loop just like the EOF path does.
   * xLineAsyncStop() belongs to the blocking xLineAsync() API and
   * is a no-op (or worse, a state-machine poke) in the stepped
   * REPL, so we don't call it here. */
  xEventLoopStop(ctx->loop);
}
