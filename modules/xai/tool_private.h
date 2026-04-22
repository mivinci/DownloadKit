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

/**
 * @brief Synchronously run the tool's handler.
 *
 * Lifetime: @p in is owned by the caller; the handler writes into
 * @p out (a caller-supplied slot) and the caller must copy any
 * pointers it wants to retain, because they may refer to per-call
 * handler-local storage.
 */
xErrno ai_tool_invoke(xAiTool tool, const xAiContent *in, xAiContent *out);

#ifdef __cplusplus
}
#endif

#endif /* XAI_TOOL_PRIVATE_H */
