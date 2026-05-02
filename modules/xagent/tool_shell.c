/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool_shell.c - Shell execution tool for the xai agent core
 *
 * Plan B: the handler returns xErrno_Pending immediately after
 * submitting the xCommandExecutor. The event loop is free to process
 * other events while the command runs.
 *
 * Output streaming:
 *   - If xAgentShellConf::on_stream is set, each stdout/stderr chunk is
 *     forwarded to it from the on_stdout/on_stderr callbacks.
 *   - The query-level on_tool_output callback is also fired for each
 *     chunk, if the caller provided one.
 *   - Output is accumulated in InvokeCtx buffers for the final result.
 *
 * Cancellation:
 *   - The on_cancel_fn callback calls xCommandExecutorCancel(), which
 *     sends SIGTERM to the child process group. The on_done callback
 *     will still fire with the final result, which is then delivered
 *     as a tool_result via ai_query_async_tool_complete().
 *
 * Memory strategy: each invocation allocates an InvokeCtx that holds
 * the accumulated output buffers and the deep-copied tool_use fields.
 * The InvokeCtx is freed after ai_query_async_tool_complete returns.
 */

#include <xagent/tool_shell.h>

#include "query_private.h"

#include <cJSON.h>
#include <xbase/command.h>
#include <xbase/log.h>
#include <xbase/map.h>
#include <xbase/string.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ───────────────────── Constants ───────────────────── */

#define DEFAULT_TIMEOUT_MS 30000
#define DEFAULT_CAP        65536

/* ───────────────────── Per-invocation context ───────────────────── */

/* Forward declaration — defined below. */
XDEF_STRUCT(ShellCtx);

XDEF_STRUCT(InvokeCtx) {
  xEventLoop       loop;
  xCommandExecutor exec;

  /* Deep-copied tool_use input (valid for the lifetime of InvokeCtx) */
  char *tool_use_id;
  char *tool_use_name;

  /* Query and tool handles (set by handler, used by on_cmd_done) */
  xAgentQuery query;
  xAgentTool  tool;

  /* Back-reference to ShellCtx for on_cmd_done to unregister from running map
   */
  ShellCtx *shell_ctx;

  /* Accumulated output */
  xString result_buf; /**< JSON result buffer                */
  xString stdout_buf; /**< Accumulated stdout                */
  xString stderr_buf; /**< Accumulated stderr                */

  /* Shell-specific callbacks */
  xAgentShellOnCommandFunc on_command;
  xAgentShellOnResultFunc  on_result;
  xAgentShellOnOutputFunc  on_stream;
  void                 *callback_ud;

  /* Config */
  size_t stdout_cap;
  size_t stderr_cap;
};

/* ───────────────────── Tool-level context ───────────────────── */

XDEF_STRUCT(ShellCtx) {
  xEventLoop       loop;
  xCommandExecutor exec;
  uint64_t         timeout_ms;
  size_t           stdout_cap;
  size_t           stderr_cap;

  xAgentShellOnCommandFunc on_command;
  xAgentShellOnResultFunc  on_result;
  xAgentShellOnOutputFunc  on_stream;
  void                 *callback_ud;

  /* Mapping from tool_use_id → InvokeCtx for running commands.
   * Used by shell_stdin to locate a running command by its tool_use_id
   * and write to its stdin fd. Populated by shell_handler, depopulated
   * by on_cmd_done. */
  xMap running;

  /* Reusable output buffer for shell_stdin_handler.
   * Cleared and refilled on each invocation; avoids strdup leak. */
  xString stdin_result_buf;
};

/* ───────────────────── JSON Schema ───────────────────── */

static const char kSchema[] =
  "{"
  "\"type\":\"object\","
  "\"properties\":{"
  "\"command\":{"
  "\"type\":\"string\","
  "\"description\":\"Shell command to execute (run via /bin/sh -c)\""
  "},"
  "\"cwd\":{"
  "\"type\":\"string\","
  "\"description\":\"Working directory for the command (default: inherit)\""
  "},"
  "\"timeout_ms\":{"
  "\"type\":\"integer\","
  "\"description\":\"Timeout in milliseconds (default: 30000)\""
  "}"
  "},"
  "\"required\":[\"command\"]"
  "}";

static const char kStdinSchema[] =
  "{"
  "\"type\":\"object\","
  "\"properties\":{"
  "\"input\":{"
  "\"type\":\"string\","
  "\"description\":\"Text to write to the running command's stdin. "
  "Add a trailing newline (\\\\n) if the command expects you to press Enter.\""
  "},"
  "\"tool_use_id\":{"
  "\"type\":\"string\","
  "\"description\":\"The tool_use_id of the still-running shell invocation. "
  "This is the id that was assigned to the tool_use block when you called "
  "the shell tool. Copy it exactly from the tool_use block you sent.\""
  "}"
  "},"
  "\"required\":[\"input\",\"tool_use_id\"]"
  "}";

/* ───────────────────── InvokeCtx lifecycle ───────────────────── */

static void invoke_ctx_destroy(InvokeCtx *ictx) {
  if (!ictx) return;
  free(ictx->tool_use_id);
  free(ictx->tool_use_name);
  xStringDestroy(ictx->result_buf);
  xStringDestroy(ictx->stdout_buf);
  xStringDestroy(ictx->stderr_buf);
  free(ictx);
}

/* ───────────────────── Helper: build JSON result ───────────────────── */

static void build_json_result(InvokeCtx *ictx, const xCommandResult *r,
                              int completed) {
  xStringClear(ictx->result_buf);
  xStringAppend(&ictx->result_buf, "{");

  if (completed && r) {
    xStringAppendFormat(&ictx->result_buf, "\"exit_code\":%d,", r->exit_code);

    /* stdout */
    if (r->stdout_buf && r->stdout_len > 0) {
      cJSON *sj  = cJSON_CreateString(r->stdout_buf);
      char  *raw = cJSON_PrintUnformatted(sj);
      xStringAppendFormat(&ictx->result_buf, "\"stdout\":%s,", raw);
      free(raw);
      cJSON_Delete(sj);
    } else {
      xStringAppend(&ictx->result_buf, "\"stdout\":\"\",");
    }

    /* stderr */
    if (r->stderr_buf && r->stderr_len > 0) {
      cJSON *sj  = cJSON_CreateString(r->stderr_buf);
      char  *raw = cJSON_PrintUnformatted(sj);
      xStringAppendFormat(&ictx->result_buf, "\"stderr\":%s,", raw);
      free(raw);
      cJSON_Delete(sj);
    } else {
      xStringAppend(&ictx->result_buf, "\"stderr\":\"\",");
    }

    xStringAppendFormat(&ictx->result_buf, "\"timed_out\":%s,",
                        r->timed_out ? "true" : "false");
    xStringAppendFormat(&ictx->result_buf, "\"elapsed_ms\":%llu",
                        (unsigned long long)r->elapsed_ms);
  } else {
    /* Command did not complete (cancelled or other) */
    xStringAppend(&ictx->result_buf,
                  "\"exit_code\":-1,"
                  "\"stdout\":\"\","
                  "\"stderr\":\"command was cancelled or failed to start\","
                  "\"timed_out\":false,"
                  "\"elapsed_ms\":0");
  }

  xStringAppend(&ictx->result_buf, "}");
}

/* ───────────────────── xCommandExecutor callbacks ───────────────────── */

static void on_cmd_stdout(xCommandExecutor exec, const char *data, size_t len,
                          void *ud) {
  (void)exec;
  InvokeCtx *ictx = (InvokeCtx *)ud;

  /* Accumulate for final result */
  if (ictx->stdout_cap == 0 ||
      xStringLen(ictx->stdout_buf) + len <= ictx->stdout_cap) {
    xStringAppendLen(&ictx->stdout_buf, data, len);
  } else if (xStringLen(ictx->stdout_buf) < ictx->stdout_cap) {
    size_t remain = ictx->stdout_cap - xStringLen(ictx->stdout_buf);
    xStringAppendLen(&ictx->stdout_buf, data, remain);
  }

  /* Forward to shell-level stream callback */
  if (ictx->on_stream) {
    ictx->on_stream(data, len, /*is_stderr=*/0, ictx->callback_ud);
  }

  /* Forward to query-level on_tool_output callback */
  if (ictx->query) {
    struct xAgentQuery_ *q = (struct xAgentQuery_ *)ictx->query;
    if (q->cbs.on_tool_output) {
      q->cbs.on_tool_output(ictx->query, ictx->tool_use_id, ictx->tool_use_name,
                            data, len, q->cbs.user_data);
    }
  }
}

static void on_cmd_stderr(xCommandExecutor exec, const char *data, size_t len,
                          void *ud) {
  (void)exec;
  InvokeCtx *ictx = (InvokeCtx *)ud;

  /* Accumulate for final result */
  if (ictx->stderr_cap == 0 ||
      xStringLen(ictx->stderr_buf) + len <= ictx->stderr_cap) {
    xStringAppendLen(&ictx->stderr_buf, data, len);
  } else if (xStringLen(ictx->stderr_buf) < ictx->stderr_cap) {
    size_t remain = ictx->stderr_cap - xStringLen(ictx->stderr_buf);
    xStringAppendLen(&ictx->stderr_buf, data, remain);
  }

  /* Forward to shell-level stream callback */
  if (ictx->on_stream) {
    ictx->on_stream(data, len, /*is_stderr=*/1, ictx->callback_ud);
  }

  /* Forward to query-level on_tool_output callback */
  if (ictx->query) {
    struct xAgentQuery_ *q = (struct xAgentQuery_ *)ictx->query;
    if (q->cbs.on_tool_output) {
      q->cbs.on_tool_output(ictx->query, ictx->tool_use_id, ictx->tool_use_name,
                            data, len, q->cbs.user_data);
    }
  }
}

static void on_cmd_done(xCommandExecutor exec, const xCommandResult *result,
                        void *ud) {
  (void)exec;
  InvokeCtx *ictx = (InvokeCtx *)ud;

  /* Build a synthetic result with accumulated output.
   * In Stream mode the executor does not capture, so we use
   * our own accumulated buffers. */
  xCommandResult final_result = *result;
  final_result.stdout_buf     = ictx->stdout_buf;
  final_result.stdout_len     = xStringLen(ictx->stdout_buf);
  final_result.stderr_buf     = ictx->stderr_buf;
  final_result.stderr_len     = xStringLen(ictx->stderr_buf);

  int completed =
    (result->exit_code >= 0 || result->timed_out || result->signaled);

  build_json_result(ictx, &final_result, completed);

  /* Unregister from running map before delivering result (which may
   * start a new tool call that could race with shell_stdin). */
  if (ictx->shell_ctx && ictx->shell_ctx->running && ictx->tool_use_id) {
    xMapDel(ictx->shell_ctx->running, ictx->tool_use_id);
  }

  /* Notify shell-level result callback */
  if (ictx->on_result) {
    ictx->on_result(final_result.exit_code, final_result.stdout_len,
                    final_result.stderr_len, final_result.timed_out,
                    ictx->callback_ud);
  }

  /* Build the xAgentContent result for ai_query_async_tool_complete */
  xAgentContent result_content               = {0};
  result_content.type                     = xAgentContentType_ToolResult;
  result_content.u.tool_result.id         = ictx->tool_use_id;
  result_content.u.tool_result.output     = ictx->result_buf;
  result_content.u.tool_result.output_len = xStringLen(ictx->result_buf);
  if (!completed) {
    result_content.u.tool_result.is_error = 1;
  }

  /* Deliver result to the Query */
  ai_query_async_tool_complete((struct xAgentQuery_ *)ictx->query,
                               ictx->tool_use_id, &result_content);

  /* Free the per-invocation context */
  invoke_ctx_destroy(ictx);
}

/* ───────────────────── Cancel callback ───────────────────── */

static void shell_on_cancel(xAgentQuery q, const char *tool_use_id, xAgentTool tool,
                            void *ud) {
  (void)q;
  (void)tool_use_id;
  (void)tool;
  ShellCtx *ctx = (ShellCtx *)ud;
  if (ctx && ctx->exec) {
    xCommandExecutorCancel(ctx->exec);
  }
}

/* ───────────────────── Handler (Plan B: async) ───────────────────── */

static xErrno shell_handler(xAgentQuery q, const xAgentContent *in, xAgentContent *out,
                            void *ud) {
  (void)out; /* async mode: result delivered via ai_query_async_tool_complete */
  ShellCtx *ctx = (ShellCtx *)ud;

  /* ── Parse args_json ──────────────────────────────────── */
  if (!in || in->type != xAgentContentType_ToolUse || !in->u.tool_use.args_json)
    return xErrno_InvalidArg;

  cJSON *root = cJSON_Parse(in->u.tool_use.args_json);
  if (!root) return xErrno_InvalidArg;

  cJSON *cmd_node = cJSON_GetObjectItemCaseSensitive(root, "command");
  cJSON *cwd_node = cJSON_GetObjectItemCaseSensitive(root, "cwd");
  cJSON *tmo_node = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");

  if (!cmd_node || !cJSON_IsString(cmd_node) || !cmd_node->valuestring[0]) {
    cJSON_Delete(root);
    return xErrno_InvalidArg;
  }

  char    *command = strdup(cmd_node->valuestring);
  char    *cwd     = (cwd_node && cJSON_IsString(cwd_node))
                       ? strdup(cwd_node->valuestring)
                       : NULL;
  uint64_t tmo     = (tmo_node && cJSON_IsNumber(tmo_node))
                       ? (uint64_t)tmo_node->valuedouble
                       : ctx->timeout_ms;

  /* cJSON tree no longer needed — command, cwd & tmo are now owned copies. */
  cJSON_Delete(root);

  if (!command) {
    free(cwd);
    return xErrno_NoMemory;
  }

  if (tmo == 0) tmo = DEFAULT_TIMEOUT_MS;

  /* ── Allocate per-invocation context ──────────────────── */
  InvokeCtx *ictx = (InvokeCtx *)calloc(1, sizeof(InvokeCtx));
  if (!ictx) {
    free(command);
    free(cwd);
    return xErrno_NoMemory;
  }

  ictx->loop          = ctx->loop;
  ictx->exec          = ctx->exec;
  ictx->query         = q;
  ictx->tool_use_id   = strdup(in->u.tool_use.id ? in->u.tool_use.id : "");
  ictx->tool_use_name = strdup(in->u.tool_use.name ? in->u.tool_use.name : "");

  if (!ictx->tool_use_id || !ictx->tool_use_name) {
    free(command);
    free(cwd);
    invoke_ctx_destroy(ictx);
    return xErrno_NoMemory;
  }

  ictx->result_buf = xStringCreate("");
  ictx->stdout_buf = xStringCreate("");
  ictx->stderr_buf = xStringCreate("");
  if (!ictx->result_buf || !ictx->stdout_buf || !ictx->stderr_buf) {
    free(command);
    free(cwd);
    invoke_ctx_destroy(ictx);
    return xErrno_NoMemory;
  }

  ictx->on_command  = ctx->on_command;
  ictx->on_result   = ctx->on_result;
  ictx->on_stream   = ctx->on_stream;
  ictx->callback_ud = ctx->callback_ud;
  ictx->stdout_cap  = ctx->stdout_cap;
  ictx->stderr_cap  = ctx->stderr_cap;
  ictx->shell_ctx   = ctx;

  /* ── Build xCommandConf ───────────────────────────────── */
  const char *argv[] = {"-c", command, NULL};

  xCommandConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.cwd          = cwd;
  conf.timeout_ms   = tmo;
  conf.stdout_cap   = ctx->stdout_cap;
  conf.stderr_cap   = ctx->stderr_cap;
  conf.stdout_mode  = xCommandOutput_Stream;
  conf.stderr_mode  = xCommandOutput_Stream;
  conf.input_mode   = xCommandInput_Pipe;

  /* ── Notify caller: command is about to run ──────────────── */
  if (ctx->on_command) ctx->on_command(command, cwd, ctx->callback_ud);

  /* ── Submit ───────────────────────────────────────────── */
  xErrno rc = xCommandExecutorSubmit(ctx->exec, &conf, on_cmd_stdout,
                                     on_cmd_stderr, on_cmd_done, ictx);

  /* command & cwd are no longer needed — the child process has its own
   * copy after fork() inside Submit. */
  free(command);
  free(cwd);

  if (rc != xErrno_Ok) {
    invoke_ctx_destroy(ictx);
    return rc;
  }

  /* Register in running map so shell_stdin can find this invocation */
  if (ctx->running && ictx->tool_use_id) {
    xMapSet(ctx->running, ictx->tool_use_id, ictx);
  }

  /* Return Pending — the event loop is free, on_cmd_done will
   * deliver the result asynchronously via ai_query_async_tool_complete. */
  return xErrno_Pending;
}

/* ───────────────────── shell_stdin handler ───────────────────── */

/**
 * shell_stdin_handler — Write input to a running shell command's stdin.
 *
 * The AI calls this tool when it wants to send input to a command
 * that was started by the "shell" tool and is still running.
 *
 * Args:
 *   input       — text to write to the child's stdin
 *   tool_use_id — the tool_use_id of the running "shell" invocation
 *
 * The handler looks up the InvokeCtx in ShellCtx::running by tool_use_id,
 * obtains the stdin fd via xCommandExecutorStdinFd(), and writes the
 * input text to it.
 *
 * Returns a JSON result: { "written": <bytes_written> }
 * On error: { "error": "<message>" }
 */
static xErrno shell_stdin_handler(xAgentQuery q, const xAgentContent *in,
                                  xAgentContent *out, void *ud) {
  (void)q;
  ShellCtx *ctx = (ShellCtx *)ud;

  /* ── Parse args_json ──────────────────────────────────── */
  if (!in || in->type != xAgentContentType_ToolUse || !in->u.tool_use.args_json)
    return xErrno_InvalidArg;

  cJSON *root = cJSON_Parse(in->u.tool_use.args_json);
  if (!root) return xErrno_InvalidArg;

  cJSON *input_node = cJSON_GetObjectItemCaseSensitive(root, "input");
  cJSON *id_node    = cJSON_GetObjectItemCaseSensitive(root, "tool_use_id");

  if (!input_node || !cJSON_IsString(input_node) || !id_node ||
      !cJSON_IsString(id_node)) {
    cJSON_Delete(root);
    return xErrno_InvalidArg;
  }

  const char *input_text  = input_node->valuestring;
  const char *tool_use_id = id_node->valuestring;

  if (!input_text || !tool_use_id || !tool_use_id[0]) {
    cJSON_Delete(root);
    return xErrno_InvalidArg;
  }

  /* ── Look up the running invocation ──────────────────── */
  InvokeCtx *ictx = (InvokeCtx *)xMapGet(ctx->running, tool_use_id);
  cJSON_Delete(root);

  out->type                   = xAgentContentType_ToolResult;
  out->u.tool_result.id       = in->u.tool_use.id;
  out->u.tool_result.is_error = 0;

  if (!ictx) {
    /* No running command with this tool_use_id */
    xStringClear(ctx->stdin_result_buf);
    xStringAppend(
      &ctx->stdin_result_buf,
      "{\"error\":\"No running shell command with this tool_use_id\"}");
    out->u.tool_result.output     = ctx->stdin_result_buf;
    out->u.tool_result.output_len = xStringLen(ctx->stdin_result_buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  /* ── Get the stdin fd and write ──────────────────────── */
  int stdin_fd = xCommandExecutorStdinFd(ictx->exec);
  if (stdin_fd < 0) {
    xStringClear(ctx->stdin_result_buf);
    xStringAppend(&ctx->stdin_result_buf,
                  "{\"error\":\"Stdin is not available for this command\"}");
    out->u.tool_result.output     = ctx->stdin_result_buf;
    out->u.tool_result.output_len = xStringLen(ctx->stdin_result_buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  size_t  input_len = strlen(input_text);
  ssize_t written   = write(stdin_fd, input_text, input_len);

  if (written < 0) {
    xStringClear(ctx->stdin_result_buf);
    xStringAppendFormat(&ctx->stdin_result_buf,
                        "{\"error\":\"write failed: %s\"}", strerror(errno));
    out->u.tool_result.output     = ctx->stdin_result_buf;
    out->u.tool_result.output_len = xStringLen(ctx->stdin_result_buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  /* ── Build success result ────────────────────────────── */
  xStringClear(ctx->stdin_result_buf);
  xStringAppendFormat(&ctx->stdin_result_buf, "{\"written\":%zd}", written);

  out->u.tool_result.output     = ctx->stdin_result_buf;
  out->u.tool_result.output_len = xStringLen(ctx->stdin_result_buf);
  out->u.tool_result.is_error   = 0;

  return xErrno_Ok;
}

/* ───────────────────── Destroy callback ───────────────────── */

static void shell_ctx_destroy(void *ud) {
  ShellCtx *ctx = (ShellCtx *)ud;
  if (!ctx) return;
  if (ctx->running) xMapDestroy(ctx->running);
  xStringDestroy(ctx->stdin_result_buf);
  xCommandExecutorDestroy(ctx->exec);
  free(ctx);
}

/* ───────────────────── Public API ───────────────────── */

XCAPI(xAgentTool) xAgentToolShellCreate(xEventLoop loop, const xAgentShellConf *conf) {
  if (!loop) return NULL;

  ShellCtx *ctx = (ShellCtx *)calloc(1, sizeof(ShellCtx));
  if (!ctx) return NULL;

  ctx->loop = loop;
  ctx->timeout_ms =
    (conf && conf->timeout_ms) ? conf->timeout_ms : DEFAULT_TIMEOUT_MS;
  ctx->stdout_cap = (conf && conf->stdout_cap) ? conf->stdout_cap : DEFAULT_CAP;
  ctx->stderr_cap = (conf && conf->stderr_cap) ? conf->stderr_cap : DEFAULT_CAP;

  ctx->on_command  = (conf) ? conf->on_command : NULL;
  ctx->on_result   = (conf) ? conf->on_result : NULL;
  ctx->on_stream   = (conf) ? conf->on_stream : NULL;
  ctx->callback_ud = (conf) ? conf->callback_ud : NULL;

  ctx->exec = xCommandExecutorCreate(loop);
  if (!ctx->exec) {
    free(ctx);
    return NULL;
  }

  ctx->running = xMapCreate(xMapType_Hash, /*cap=*/8, xMapStrHash, xMapStrEq);
  if (!ctx->running) {
    xCommandExecutorDestroy(ctx->exec);
    free(ctx);
    return NULL;
  }

  ctx->stdin_result_buf = xStringCreate("");
  if (!ctx->stdin_result_buf) {
    xMapDestroy(ctx->running);
    xCommandExecutorDestroy(ctx->exec);
    free(ctx);
    return NULL;
  }

  xAgentToolConf tconf = {};
  tconf.name        = "shell";
  tconf.description =
    "Execute a shell command via /bin/sh -c and return the output. "
    "Output is streamed in real-time while the command runs. "
    "IMPORTANT: If the command is interactive and waits for input "
    "(e.g. prompts like 'Name:', 'Press Y/N:', a REPL, or a password), "
    "you MUST call the shell_stdin tool to send input to it — "
    "do NOT wait or assume it will time out. "
    "When calling shell_stdin, pass the tool_use_id of THIS shell "
    "invocation (the id assigned to the tool_use block you sent).";
  tconf.json_schema       = kSchema;
  tconf.handler           = shell_handler;
  tconf.user_data         = ctx;
  tconf.user_data_destroy = shell_ctx_destroy;
  tconf.concurrent_safe   = 1; /* Plan B: does not block the event loop */
  tconf.needs_confirm     = 1;
  tconf.on_done_fn        = NULL; /* Not needed: we call
                                     ai_query_async_tool_complete directly */
  tconf.on_cancel_fn = shell_on_cancel;
  tconf.on_cancel_ud = ctx;

  xAgentTool tool = xAgentToolCreate(&tconf);
  if (!tool) {
    xCommandExecutorDestroy(ctx->exec);
    free(ctx);
    return NULL;
  }

  return tool;
}

XCAPI(xAgentTool) xAgentToolShellStdinCreate(xAgentTool shell_tool) {
  if (!shell_tool) return NULL;

  ShellCtx *ctx = (ShellCtx *)xAgentToolUserData(shell_tool);
  if (!ctx) return NULL;

  xAgentToolConf tconf = {};
  tconf.name        = "shell_stdin";
  tconf.description =
    "Send input text to a running shell command's stdin. "
    "Use this when the shell command is waiting for input "
    "(e.g. a prompt, a confirmation question, a REPL, or a password). "
    "You MUST provide: (1) tool_use_id — the id from the tool_use block "
    "of the running shell invocation, and (2) input — the text to write. "
    "If the command expects Enter to be pressed, include a trailing "
    "newline (\\n) in the input text. "
    "You can call this tool multiple times for multi-step interactions.";
  tconf.json_schema       = kStdinSchema;
  tconf.handler           = shell_stdin_handler;
  tconf.user_data         = ctx;
  tconf.user_data_destroy = NULL; /* ShellCtx is owned by shell_tool */
  tconf.concurrent_safe   = 1;
  tconf.needs_confirm     = 0;

  xAgentTool tool = xAgentToolCreate(&tconf);
  if (!tool) return NULL;

  return tool;
}
