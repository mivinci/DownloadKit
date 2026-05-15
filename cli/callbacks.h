/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * callbacks.h - xAgentSession callbacks.
 *
 * Every function in this header is installed into an xAgentSessionConf
 * in main(). They each take `void *ud` which is cast back to
 * ReplCtx*. See callbacks.cpp for the streaming / confirm / budget
 * plumbing details.
 */

#ifndef MOO_APPS_CLI_CALLBACKS_H
#define MOO_APPS_CLI_CALLBACKS_H

#include <cstddef>

#include <xagent/session.h>
#include <xbase/error.h>

void on_text(xAgentSession sess, const char *chunk, size_t len, void *ud);
void on_thinking(xAgentSession sess, const char *chunk, size_t len, void *ud);
void on_tool(xAgentSession sess, const char *tool_name, int started, void *ud);
void on_tool_output(xAgentSession sess, const char *tool_use_id,
                    const char *tool_name, const char *data, size_t len,
                    void *ud);
void on_sidecar(xAgentSession sess, xAgentSidecarEvent event, void *ud);
void on_done(xAgentSession sess, xAgentDoneReason reason,
             const xAgentUsage *usage, void *ud);
void on_error(xAgentSession sess, xErrno err, const char *msg, void *ud);
void on_budget_event(xAgentSession sess, xAgentBudgetEvent event,
                     const void *info, void *ud);

#endif /* MOO_APPS_CLI_CALLBACKS_H */
