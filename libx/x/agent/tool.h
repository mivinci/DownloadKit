/*
 * Copyright 2025 The libx Authors. All rights reserved.
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
 * - Tools are opaque handles created with xAgentToolCreate; the agent
 *   borrows them (it does not take ownership). The caller destroys
 *   each tool with xAgentToolDestroy after all agents/sessions using it
 *   are gone.
 *
 * - Handlers receive and produce xAgentContent (see <x/agent/message.h>):
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

#ifndef XAGENT_TOOL_H
#define XAGENT_TOOL_H

#include <stddef.h>
#include <x/agent/message.h>
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Opaque handle to a running query (forward declaration).
 *
 * Full definition in <x/agent/query.h>. Declared here so that
 * xAgentToolDoneFunc / xAgentToolCancelFunc can accept a Query pointer
 * without pulling in the entire query.h header.
 */
XDEF_HANDLE(xAgentQuery);

/**
 * @brief Opaque handle to a registered tool.
 */
XDEF_HANDLE(xAgentTool);

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
 *   until the surrounding xAgentToolHandlerFunc call returns. The
 *   agent copies @p out->u.tool_result fields before dispatching
 *   the result back to the provider.
 *
 * Threading:
 * - If the tool is @ref concurrent_safe, the handler may be called
 *   on a worker thread from the agent's task pool.
 * - Otherwise it is invoked on the event loop thread.
 *
 * @param q    The Query that dispatched this tool call.
 * @param in   The tool_use content block (type == ToolUse).
 * @param out  Output slot to be filled with a tool_result content
 *             block (handler must set out->type and out->u.tool_result
 *             for synchronous completion; for async see @ref on_done_fn).
 * @param ud   The user_data pointer supplied in xAgentToolConf.
 * @return     xErrno_Ok on synchronous success; xErrno_Pending if the
 *             operation was submitted asynchronously (the agent will
 *             be notified of completion via @ref on_done_fn); anything
 *             else is surfaced as a tool error to the model.
 */
typedef xErrno (*xAgentToolHandlerFunc)(xAgentQuery q, const xAgentContent *in,
                                     xAgentContent *out, void *ud);

typedef void (*xAgentToolUserDataDestroyFunc)(void *user_data);

/**
 * @brief Async completion callback for tools that return xErrno_Pending.
 *
 * When a tool handler returns xErrno_Pending, the agent will call
 * this function once the operation completes. The callback must
 * populate @p out with a valid tool_result content block and then
 * call ai_query_async_tool_complete() to notify the query that the
 * tool has finished.
 *
 * @param q          The Query that dispatched this tool call.
 * @param tool_use_id  The tool_use_id from the original tool_use content.
 * @param tool       The tool handle.
 * @param in         The original tool_use content block (deep-copied by the
 *                   agent before the async dispatch; valid for the duration
 *                   of this callback only).
 * @param out        Output slot to be filled with the completed tool_result
 *                   content block.
 * @param ud         The on_done_ud pointer supplied in xAgentToolConf.
 */
typedef void (*xAgentToolDoneFunc)(xAgentQuery q, const char *tool_use_id,
                                xAgentTool tool, const xAgentContent *in,
                                xAgentContent *out, void *ud);

/**
 * @brief Cancellation callback for async tools.
 *
 * When a Query is cancelled while an async tool is in-flight, the
 * agent calls this function so the tool can abort its operation
 * (e.g. kill a child process). After cancellation the tool must
 * still call ai_query_async_tool_complete() to deliver a (possibly
 * error) tool_result.
 *
 * May be NULL = the tool does not support cancellation.
 *
 * @param q          The Query being cancelled.
 * @param tool_use_id  The tool_use_id of the in-flight operation.
 * @param tool       The tool handle.
 * @param ud         The on_cancel_ud pointer supplied in xAgentToolConf.
 */
typedef void (*xAgentToolCancelFunc)(xAgentQuery q, const char *tool_use_id,
                                  xAgentTool tool, void *ud);

/**
 * @brief Configuration for creating a tool.
 *
 * Zero-initialise for defaults (not concurrent-safe, no confirmation
 * required, no user data).
 */
XDEF_STRUCT(xAgentToolConf) {
  const char *name;         /**< Tool identifier exposed to the model
                                 (must not be NULL, conventionally
                                 [a-zA-Z0-9_]+).                         */
  const char *description;  /**< Human-readable description (may be NULL)*/
  const char *json_schema;  /**< JSON Schema string for the tool's
                                 arguments (may be NULL = no arguments). */

  xAgentToolHandlerFunc handler;    /**< Execution callback (must not be NULL) */
  void              *user_data;  /**< Forwarded to @ref handler             */

  xAgentToolUserDataDestroyFunc user_data_destroy; /**< Called by
                            xAgentToolDestroy to release @ref user_data
                            (may be NULL = no cleanup needed).              */

  int concurrent_safe; /**< Non-zero: handler may run in parallel with
                            other concurrent-safe tools.                   */
  int needs_confirm;   /**< Non-zero: agent must obtain user confirmation
                            before invoking the handler (honoured once the
                            confirmation flow lands; see TODO.md).         */

  xAgentToolDoneFunc on_done_fn; /**< Async completion callback (may be NULL
                                   = synchronous tool). When non-NULL the
                                   handler may return xErrno_Pending to
                                   indicate the operation is in progress;
                                   the agent will invoke this callback with
                                   the completed tool_result once the
                                   operation finishes.                     */
  void *on_done_ud;            /**< User data forwarded to on_done_fn
                                    (may be NULL).                         */

  xAgentToolCancelFunc on_cancel_fn; /**< Cancellation callback (may be NULL
                                       = tool does not support cancel).    */
  void *on_cancel_ud;             /**< User data forwarded to on_cancel_fn
                                       (may be NULL).                      */
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
XCAPI(xAgentTool) xAgentToolCreate(const xAgentToolConf *conf);

/**
 * @brief Destroy a tool and release its resources.
 *
 * The caller must ensure no agent or session is still holding a
 * reference to @p tool.
 *
 * @param tool  Tool handle (NULL is a no-op).
 */
XCAPI(void) xAgentToolDestroy(xAgentTool tool);

/**
 * @brief Return the user_data pointer associated with a tool.
 *
 * Useful when a companion tool (e.g. shell_stdin) needs to share
 * the same context as the primary tool (e.g. shell).
 *
 * @param tool  Tool handle (NULL-safe).
 * @return      The user_data pointer, or NULL.
 */
XCAPI(void *) xAgentToolUserData(xAgentTool tool);

#endif /* XAGENT_TOOL_H */
