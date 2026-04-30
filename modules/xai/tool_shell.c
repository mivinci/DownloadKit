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
 *   - If xAiShellConf::on_stream is set, each stdout/stderr chunk is
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

#include <xai/tool_shell.h>

#include "query_private.h"

#include <cJSON.h>
#include <xbase/command.h>
#include <xbase/string.h>

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Constants ───────────────────── */

#define DEFAULT_TIMEOUT_MS 30000
#define DEFAULT_CAP        65536

/* ───────────────────── Per-invocation context ───────────────────── */

XDEF_STRUCT(InvokeCtx) {
  xEventLoop       loop;
  xCommandExecutor exec;

  /* Deep-copied tool_use input (valid for the lifetime of InvokeCtx) */
  char            *tool_use_id;
  char            *tool_use_name;

  /* Query and tool handles (set by handler, used by on_cmd_done) */
  xAiQuery         query;
  xAiTool          tool;

  /* Accumulated output */
  xString          result_buf;   /**< JSON result buffer                */
  xString          stdout_buf;   /**< Accumulated stdout                */
  xString          stderr_buf;   /**< Accumulated stderr                */

  /* Shell-specific callbacks */
  xAiShellOnCommandFunc on_command;
  xAiShellOnResultFunc  on_result;
  xAiShellOnOutputFunc  on_stream;
  void                 *callback_ud;

  /* Config */
  size_t           stdout_cap;
  size_t           stderr_cap;
};

/* ───────────────────── Tool-level context ───────────────────── */

XDEF_STRUCT(ShellCtx) {
  xEventLoop       loop;
  xCommandExecutor exec;
  uint64_t         timeout_ms;
  size_t           stdout_cap;
  size_t           stderr_cap;

  xAiShellOnCommandFunc on_command;
  xAiShellOnResultFunc  on_result;
  xAiShellOnOutputFunc  on_stream;
  void                 *callback_ud;
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
    struct xAiQuery_ *q = (struct xAiQuery_ *)ictx->query;
    if (q->cbs.on_tool_output) {
      q->cbs.on_tool_output(ictx->query, ictx->tool_use_id,
                            ictx->tool_use_name, data, len,
                            q->cbs.user_data);
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
    struct xAiQuery_ *q = (struct xAiQuery_ *)ictx->query;
    if (q->cbs.on_tool_output) {
      q->cbs.on_tool_output(ictx->query, ictx->tool_use_id,
                            ictx->tool_use_name, data, len,
                            q->cbs.user_data);
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
  final_result.stdout_buf = ictx->stdout_buf;
  final_result.stdout_len = xStringLen(ictx->stdout_buf);
  final_result.stderr_buf = ictx->stderr_buf;
  final_result.stderr_len = xStringLen(ictx->stderr_buf);

  int completed = (result->exit_code >= 0 || result->timed_out ||
                   result->signaled);

  build_json_result(ictx, &final_result, completed);

  /* Notify shell-level result callback */
  if (ictx->on_result) {
    ictx->on_result(final_result.exit_code, final_result.stdout_len,
                    final_result.stderr_len, final_result.timed_out,
                    ictx->callback_ud);
  }

  /* Build the xAiContent result for ai_query_async_tool_complete */
  xAiContent result_content = {0};
  result_content.type                     = xAiContentType_ToolResult;
  result_content.u.tool_result.id         = ictx->tool_use_id;
  result_content.u.tool_result.output     = ictx->result_buf;
  result_content.u.tool_result.output_len = xStringLen(ictx->result_buf);
  if (!completed) {
    result_content.u.tool_result.is_error = 1;
  }

  /* Deliver result to the Query */
  ai_query_async_tool_complete((struct xAiQuery_ *)ictx->query,
                               ictx->tool_use_id, &result_content);

  /* Free the per-invocation context */
  invoke_ctx_destroy(ictx);
}

/* ───────────────────── Cancel callback ───────────────────── */

static void shell_on_cancel(xAiQuery q, const char *tool_use_id,
                            xAiTool tool, void *ud) {
  (void)q;
  (void)tool_use_id;
  (void)tool;
  ShellCtx *ctx = (ShellCtx *)ud;
  if (ctx && ctx->exec) {
    xCommandExecutorCancel(ctx->exec);
  }
}

/* ───────────────────── Handler (Plan B: async) ───────────────────── */

static xErrno shell_handler(xAiQuery q, const xAiContent *in, xAiContent *out,
                            void *ud) {
  (void)out; /* async mode: result delivered via ai_query_async_tool_complete */
  ShellCtx *ctx = (ShellCtx *)ud;

  /* ── Parse args_json ──────────────────────────────────── */
  if (!in || in->type != xAiContentType_ToolUse || !in->u.tool_use.args_json)
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

  const char *command = cmd_node->valuestring;
  const char *cwd =
    (cwd_node && cJSON_IsString(cwd_node)) ? cwd_node->valuestring : NULL;
  uint64_t tmo = (tmo_node && cJSON_IsNumber(tmo_node))
                   ? (uint64_t)tmo_node->valuedouble
                   : ctx->timeout_ms;
  if (tmo == 0) tmo = DEFAULT_TIMEOUT_MS;

  /* ── Allocate per-invocation context ──────────────────── */
  InvokeCtx *ictx = (InvokeCtx *)calloc(1, sizeof(InvokeCtx));
  if (!ictx) {
    cJSON_Delete(root);
    return xErrno_NoMemory;
  }

  ictx->loop           = ctx->loop;
  ictx->exec           = ctx->exec;
  ictx->query          = q;
  ictx->tool_use_id    = strdup(in->u.tool_use.id ? in->u.tool_use.id : "");
  ictx->tool_use_name  = strdup(in->u.tool_use.name ? in->u.tool_use.name : "");

  if (!ictx->tool_use_id || !ictx->tool_use_name) {
    invoke_ctx_destroy(ictx);
    cJSON_Delete(root);
    return xErrno_NoMemory;
  }

  ictx->result_buf  = xStringCreate("");
  ictx->stdout_buf  = xStringCreate("");
  ictx->stderr_buf  = xStringCreate("");
  if (!ictx->result_buf || !ictx->stdout_buf || !ictx->stderr_buf) {
    invoke_ctx_destroy(ictx);
    cJSON_Delete(root);
    return xErrno_NoMemory;
  }

  ictx->on_command  = ctx->on_command;
  ictx->on_result   = ctx->on_result;
  ictx->on_stream   = ctx->on_stream;
  ictx->callback_ud = ctx->callback_ud;
  ictx->stdout_cap  = ctx->stdout_cap;
  ictx->stderr_cap  = ctx->stderr_cap;

  cJSON_Delete(root);

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
  xErrno rc =
    xCommandExecutorSubmit(ctx->exec, &conf,
                           on_cmd_stdout, on_cmd_stderr,
                           on_cmd_done, ictx);
  if (rc != xErrno_Ok) {
    invoke_ctx_destroy(ictx);
    return rc;
  }

  /* Return Pending — the event loop is free, on_cmd_done will
   * deliver the result asynchronously via ai_query_async_tool_complete. */
  return xErrno_Pending;
}

/* ───────────────────── Destroy callback ───────────────────── */

static void shell_ctx_destroy(void *ud) {
  ShellCtx *ctx = (ShellCtx *)ud;
  if (!ctx) return;
  xCommandExecutorDestroy(ctx->exec);
  free(ctx);
}

/* ───────────────────── Public API ───────────────────── */

XCAPI(xAiTool) xAiToolShellCreate(xEventLoop loop, const xAiShellConf *conf) {
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

  xAiToolConf tconf = {};
  tconf.name        = "shell";
  tconf.description =
    "Execute a shell command via /bin/sh -c and return the output";
  tconf.json_schema       = kSchema;
  tconf.handler           = shell_handler;
  tconf.user_data         = ctx;
  tconf.user_data_destroy = shell_ctx_destroy;
  tconf.concurrent_safe   = 1;  /* Plan B: does not block the event loop */
  tconf.needs_confirm     = 1;
  tconf.on_done_fn        = NULL;  /* Not needed: we call
                                      ai_query_async_tool_complete directly */
  tconf.on_cancel_fn      = shell_on_cancel;
  tconf.on_cancel_ud      = ctx;

  xAiTool tool = xAiToolCreate(&tconf);
  if (!tool) {
    xCommandExecutorDestroy(ctx->exec);
    free(ctx);
    return NULL;
  }

  return tool;
}
