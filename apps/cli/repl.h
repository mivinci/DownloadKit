/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * repl.h - Async REPL glue: editor lifecycle, tool-confirm gate,
 *          line dispatch, and the SIGINT watcher.
 *
 * See repl.cpp for the full rationale (editor kept alive during AI
 * runs so /cancel stays interactive, confirm queue drained on error,
 * etc).
 */

#ifndef MOO_APPS_CLI_REPL_H
#define MOO_APPS_CLI_REPL_H

#include "ctx.h"

#include <xagent/session.h>
#include <xagent/tool.h>
#include <xbase/error.h>

/* Open / close the async xline editor with the loop-registered fd.
 * `repl_open_line` uses an empty prompt (chat mode); _with_prompt is
 * used to swap to "confirm> " temporarily. */
int  repl_open_line(ReplCtx *ctx);
int  repl_open_line_with_prompt(ReplCtx *ctx, const char *prompt);
void repl_close_line(ReplCtx *ctx);

/* Submit a user chat message. Handles the Busy → compact → retry
 * dance: on Busy the text is stashed and on_budget_event(CompactDone)
 * calls back in to re-submit. Exposed because the budget callback
 * (in callbacks.cpp) is the other legitimate entry point. */
xErrno repl_submit_text(ReplCtx *ctx, const char *text);

/* Reject every queued confirm and leave confirm mode. Used from
 * /cancel, ^C, and on_done teardown paths. Safe to call even when
 * the queue is empty or confirm mode is inactive. */
void repl_drain_confirms_rejected(ReplCtx *ctx, const char *reason);

/* on_tool_confirm session callback — exported because it's installed
 * into xAgentSessionConf.cbs in main(). */
void on_tool_confirm(xAgentSession sess, const char *tool_name,
                     const char *tool_use_id, const char *args_json,
                     xAgentToolConfirmResolver resolver, void *ud);

/* Signal watcher for out-of-band SIGINT (kill -INT etc). The normal
 * in-band ^C arrives through xLineStep as XLINE_STEP_INTERRUPT. */
void repl_on_sigint(int signo, void *arg);

#endif /* MOO_APPS_CLI_REPL_H */
