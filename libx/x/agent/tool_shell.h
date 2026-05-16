/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool_shell.h - Shell execution tool for the xai agent core
 *
 * Provides a "shell" tool that lets the LLM execute arbitrary
 * commands via /bin/sh -c. Internally backed by xCommandExecutor;
 * the handler returns xErrno_Pending and delivers the result
 * asynchronously via the on_done_fn callback (Plan B — async).
 *
 * Streaming output is delivered via on_tool_output on the Query
 * callbacks and on_stream on xAgentShellConf.
 *
 * The tool IS concurrent_safe (it does not block the event loop)
 * and is marked needs_confirm (the model should not execute
 * arbitrary commands without user approval).
 *
 * Cancellation is supported: when the Query is cancelled, the
 * on_cancel_fn callback sends SIGTERM to the child process.
 */

#ifndef XAGENT_TOOL_SHELL_H
#define XAGENT_TOOL_SHELL_H

#include <stddef.h>
#include <stdint.h>
#include <x/agent/tool.h>
#include <x/base/base.h>
#include <x/base/error.h>
#include <x/base/event.h>

/**
 * @brief Called before the shell tool executes a command.
 *
 * Intended for UI affordances (e.g. echoing the command being run).
 * May be NULL (no notification).
 *
 * @param command  The shell command string (via /bin/sh -c).
 * @param cwd      Working directory (may be NULL = inherit).
 * @param ud       The callback_ud pointer from xAgentShellConf.
 */
typedef void (*xAgentShellOnCommandFunc)(const char *command, const char *cwd,
                                      void *ud);

/**
 * @brief Called after the shell command finishes.
 *
 * Intended for UI affordances (e.g. showing result length).
 * May be NULL (no notification).
 *
 * @param exit_code    Process exit code (-1 if timed out).
 * @param stdout_len   Number of bytes captured on stdout.
 * @param stderr_len   Number of bytes captured on stderr.
 * @param timed_out    Non-zero if the command exceeded its timeout.
 * @param ud           The callback_ud pointer from xAgentShellConf.
 */
typedef void (*xAgentShellOnResultFunc)(int exit_code, size_t stdout_len,
                                     size_t stderr_len, int timed_out,
                                     void *ud);

/**
 * @brief Called for each chunk of streaming output from the running command.
 *
 * Only fires in Stream mode. May be NULL (no streaming).
 *
 * @param data  Output chunk (NOT NUL-terminated).
 * @param len   Length of @p data in bytes.
 * @param is_stderr  Non-zero if this chunk came from stderr.
 * @param ud    The callback_ud pointer from xAgentShellConf.
 */
typedef void (*xAgentShellOnOutputFunc)(const char *data, size_t len,
                                     int is_stderr, void *ud);

/**
 * @brief Configuration for xAgentToolShellCreate().
 *
 * Zero-initialise for defaults.
 */
XDEF_STRUCT(xAgentShellConf) {
  uint64_t timeout_ms; /**< Timeout in ms (0 = 30000)                 */
  size_t   stdout_cap; /**< Max stdout bytes to capture (0 = 65536)   */
  size_t   stderr_cap; /**< Max stderr bytes to capture (0 = 65536)   */

  xAgentShellOnCommandFunc on_command;  /**< Optional: called before exec    */
  xAgentShellOnResultFunc  on_result;   /**< Optional: called after exec     */
  xAgentShellOnOutputFunc  on_stream;   /**< Optional: streaming output       */
  void                 *callback_ud; /**< Forwarded to on_command/on_result/on_stream */
};

/**
 * @brief Create a "shell" tool bound to the given event loop.
 *
 * The tool uses xCommandExecutor internally. Each invocation:
 *   1. Parses the "command" (required), "cwd" (optional), and
 *      "timeout_ms" (optional) arguments from the tool_use JSON.
 *   2. Spawns /bin/sh -c "<command>" via xCommandExecutorSubmit().
 *   3. Returns xErrno_Pending (async mode); the event loop is NOT
 *      blocked while the command runs.
 *   4. Streams output via on_tool_output (if the caller provides it)
 *      and on_stream (if conf provides it).
 *   5. On completion, delivers the tool_result via the on_done_fn
 *      callback, with JSON fields: exit_code, stdout, stderr,
 *      timed_out, and elapsed_ms.
 *   6. Supports cancellation: on_cancel_fn sends SIGTERM to the
 *      child process group.
 *
 * @param loop  Event loop (must not be NULL).
 * @param conf  Optional configuration (NULL for defaults).
 * @return      A new xAgentTool handle, or NULL on failure.
 */
XCAPI(xAgentTool) xAgentToolShellCreate(xEventLoop loop, const xAgentShellConf *conf);

/**
 * @brief Create a "shell_stdin" tool for sending input to running shells.
 *
 * This tool works in conjunction with the "shell" tool created by
 * xAgentToolShellCreate(). When the AI wants to interact with a running
 * command (e.g. answering a prompt, providing input to a REPL), it
 * calls "shell_stdin" with the tool_use_id of the running shell
 * invocation and the text to write.
 *
 * Both tools MUST share the same ShellCtx (created by xAgentToolShellCreate)
 * so that shell_stdin can locate the running command by its tool_use_id.
 *
 * The tool_use_id → command mapping is maintained automatically:
 *   - When a "shell" invocation starts, the mapping is registered.
 *   - When the command finishes, the mapping is removed.
 *
 * JSON schema for the tool:
 *   {
 *     "type": "object",
 *     "properties": {
 *       "input":       { "type": "string",
 *                        "description": "Text to write to the running
 *                         command's stdin" },
 *       "tool_use_id": { "type": "string",
 *                        "description": "The tool_use_id of the still-running
 *                         shell invocation to send input to. This is the same
 *                         id that the shell tool call was assigned when you
 *                         invoked it (visible in the tool_use block you sent).
 *                         Copy it exactly." }
 *     },
 *     "required": ["input", "tool_use_id"]
 *   }
 *
 * @param shell_tool  An existing shell tool handle (from xAgentToolShellCreate).
 *                    Must not be NULL.
 * @return            A new xAgentTool handle for "shell_stdin", or NULL on failure.
 */
XCAPI(xAgentTool) xAgentToolShellStdinCreate(xAgentTool shell_tool);

#endif /* XAGENT_TOOL_SHELL_H */
