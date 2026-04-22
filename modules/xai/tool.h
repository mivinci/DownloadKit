/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool.h - Tool registration for the xai agent core
 *
 * A tool is a piece of callable capability exposed to the LLM. The
 * agent collects a set of tools, advertises them to the provider
 * (via JSON Schema), and dispatches model-issued tool calls back to
 * their registered handlers.
 *
 * Design notes:
 *
 * - Tools are opaque handles created with xAiToolCreate; the agent
 *   borrows them (it does not take ownership). The caller destroys
 *   each tool with xAiToolDestroy after all agents/sessions using it
 *   are gone.
 *
 * - Handlers receive and produce xAiContent (see <xai/message.h>):
 *   the input is a ToolUse content, and the handler must fill a
 *   ToolResult content. This keeps the session-facing API free of
 *   tool-specific structures.
 *
 * - Concurrency: a tool flagged as @ref concurrent_safe may be
 *   invoked in parallel with other concurrent-safe tools by the
 *   agent scheduler. Everything else is serialised.
 *
 * - Permission: a tool flagged as @ref needs_confirm will require a
 *   user confirmation step before execution. Hook wiring is tracked
 *   in TODO.md (MVP does not implement the confirmation flow).
 */

#ifndef XAI_TOOL_H
#define XAI_TOOL_H

#include <stddef.h>
#include <xai/message.h>
#include <xbase/base.h>
#include <xbase/error.h>

/**
 * @brief Opaque handle to a registered tool.
 */
XDEF_HANDLE(xAiTool);

/**
 * @brief Tool execution handler.
 *
 * Invoked by the agent when the model issues a tool call matching
 * the tool's @ref name. The handler receives a ToolUse content
 * block and must populate a ToolResult content block via @p out.
 *
 * Lifetime:
 * - @p in is valid only for the duration of the call; the handler
 *   must copy anything it needs to keep.
 * - Any pointer the handler writes into @p out must remain valid
 *   until the surrounding xAiToolHandlerFunc call returns. The
 *   agent copies @p out->u.tool_result fields before dispatching
 *   the result back to the provider.
 *
 * Threading:
 * - If the tool is @ref concurrent_safe, the handler may be called
 *   on a worker thread from the agent's task pool.
 * - Otherwise it is invoked on the event loop thread.
 *
 * @param in   The tool_use content block (type == ToolUse).
 * @param out  Output slot to be filled with a tool_result content
 *             block (handler must set out->type and out->u.tool_result).
 * @param ud   The user_data pointer supplied in xAiToolConf.
 * @return     xErrno_Ok on success; anything else is surfaced as a
 *             tool error to the model.
 */
typedef xErrno (*xAiToolHandlerFunc)(const xAiContent *in, xAiContent *out,
                                     void *ud);

/**
 * @brief Configuration for creating a tool.
 *
 * Zero-initialise for defaults (not concurrent-safe, no confirmation
 * required, no user data).
 */
XDEF_STRUCT(xAiToolConf) {
  const char *name;         /**< Tool identifier exposed to the model
                                 (must not be NULL, conventionally
                                 [a-zA-Z0-9_]+).                         */
  const char *description;  /**< Human-readable description (may be NULL)*/
  const char *json_schema;  /**< JSON Schema string for the tool's
                                 arguments (may be NULL = no arguments). */

  xAiToolHandlerFunc handler;    /**< Execution callback (must not be NULL) */
  void              *user_data;  /**< Forwarded to @ref handler             */

  int concurrent_safe; /**< Non-zero: handler may run in parallel with
                            other concurrent-safe tools.                   */
  int needs_confirm;   /**< Non-zero: agent must obtain user confirmation
                            before invoking the handler (honoured once the
                            confirmation flow lands; see TODO.md).         */
};

/**
 * @brief Create a tool from the given configuration.
 *
 * The tool snapshots the fields of @p conf that it needs internally;
 * the caller may reuse @p conf immediately after.
 *
 * @param conf  Tool configuration (must not be NULL, conf->name and
 *              conf->handler must not be NULL).
 * @return      A new tool handle, or NULL on failure.
 */
XCAPI(xAiTool) xAiToolCreate(const xAiToolConf *conf);

/**
 * @brief Destroy a tool and release its resources.
 *
 * The caller must ensure no agent or session is still holding a
 * reference to @p tool.
 *
 * @param tool  Tool handle (NULL is a no-op).
 */
XCAPI(void) xAiToolDestroy(xAiTool tool);

#endif /* XAI_TOOL_H */
