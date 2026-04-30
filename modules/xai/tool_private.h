/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool_private.h - Internal accessors for xAiTool
 *
 * xAiTool is an opaque handle to external users; the fields it holds
 * (name, description, json_schema, concurrent_safe flag, ...) must
 * only be reached through these helpers from within the xai module.
 *
 * Not installed, not part of the public API. Only consumed by
 * sibling TUs in modules/xai (tool.c, provider_openai.c, session.c).
 */

#ifndef XAI_TOOL_PRIVATE_H
#define XAI_TOOL_PRIVATE_H

#include <xai/message.h>
#include <xai/tool.h>
#include <xbase/error.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *ai_tool_name(xAiTool tool);
const char *ai_tool_description(xAiTool tool);
const char *ai_tool_json_schema(xAiTool tool);

int ai_tool_concurrent_safe(xAiTool tool);
int ai_tool_needs_confirm(xAiTool tool);

xAiToolDoneFunc ai_tool_on_done_fn(xAiTool tool);
void *ai_tool_on_done_ud(xAiTool tool);

xAiToolCancelFunc ai_tool_on_cancel_fn(xAiTool tool);
void *ai_tool_on_cancel_ud(xAiTool tool);

/**
 * @brief Run the tool's handler.
 *
 * Lifetime: @p in is owned by the caller; the handler writes into
 * @p out (a caller-supplied slot) and the caller must copy any
 * pointers it wants to retain, because they may refer to per-call
 * handler-local storage.
 *
 * Return values:
 * - xErrno_Ok: synchronous completion — @p out is populated with a
 *   valid tool_result content block.
 * - xErrno_Pending: asynchronous execution — the handler has submitted
 *   the operation and will call ai_query_async_tool_complete() when
 *   it completes. @p out is NOT populated; the agent will supply the
 *   tool_result via the completion path instead.
 * - Any other xErrno: tool error, surfaced to the model.
 */
xErrno ai_tool_invoke(xAiTool tool, xAiQuery q, const xAiContent *in,
                      xAiContent *out);

#ifdef __cplusplus
}
#endif

#endif /* XAI_TOOL_PRIVATE_H */
