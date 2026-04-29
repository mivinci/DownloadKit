/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool_shell.c - Shell execution tool for the xai agent core
 *
 * Plan A: the handler blocks the event loop with xEventLoopWait()
 * while xCommandExecutor runs the child. When on_done fires it
 * calls xEventLoopStop() to unblock the handler, which then fills
 * the tool_result and returns.
 *
 * Memory strategy: the JSON result is built into ShellCtx::result_buf
 * (an xStr). The handler borrows the pointer for the duration of the
 * ai_tool_invoke() call — the caller (query.c) deep-copies the
 * output before returning, so the xStr only needs to survive that
 * window. On the next invocation the buffer is cleared and reused;
 * on tool destruction it is freed.
 */

#include <xai/tool_shell.h>

#include <cJSON.h>
#include <xbase/cmd.h>
#include <xbase/str.h>

#include <stdlib.h>
#include <string.h>

/* ───────────────────── Constants ───────────────────── */

#define DEFAULT_TIMEOUT_MS 30000
#define DEFAULT_CAP        65536

/* ───────────────────── Context ───────────────────── */

XDEF_STRUCT(ShellCtx) {
  xEventLoop       loop;
  xCommandExecutor exec;
  uint64_t         timeout_ms;
  size_t           stdout_cap;
  size_t           stderr_cap;

  xStr result_buf; /**< Reusable buffer for JSON output */

  xAiShellOnCommandFunc on_command;
  xAiShellOnResultFunc  on_result;
  void                 *callback_ud;

  /* Filled by on_done, consumed by handler */
  const xCommandResult *result; /* borrowed, valid inside on_done only */
  int                   done;   /* non-zero once on_done has fired     */
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

/* ───────────────────── on_done callback ───────────────────── */

static void on_cmd_done(xCommandExecutor exec, const xCommandResult *result,
                        void *ud) {
  (void)exec;
  ShellCtx *ctx = (ShellCtx *)ud;
  ctx->result   = result;
  ctx->done     = 1;
  xEventLoopStop(ctx->loop);
}

/* ───────────────────── Handler ───────────────────── */

static xErrno shell_handler(const xAiContent *in, xAiContent *out, void *ud) {
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

  /* ── Build xCommandConf ───────────────────────────────── */
  const char *argv[] = {"-c", command, NULL};

  xCommandConf conf = {};
  conf.cmd          = "/bin/sh";
  conf.argv         = argv;
  conf.cwd          = cwd;
  conf.timeout_ms   = tmo;
  conf.stdout_cap   = ctx->stdout_cap;
  conf.stderr_cap   = ctx->stderr_cap;
  conf.stdout_mode  = xCommandOutput_Capture;
  conf.stderr_mode  = xCommandOutput_Capture;
  conf.input_mode   = xCommandInput_Pipe;

  /* ── Notify caller: command is about to run ──────────────── */
  if (ctx->on_command) ctx->on_command(command, cwd, ctx->callback_ud);

  /* ── Submit and block ─────────────────────────────────── */
  ctx->done   = 0;
  ctx->result = NULL;

  xErrno rc =
    xCommandExecutorSubmit(ctx->exec, &conf, NULL, NULL, on_cmd_done, ctx);
  if (rc != xErrno_Ok) {
    cJSON_Delete(root);
    return rc;
  }

  /* Block the event loop until on_done fires (or timeout + 2s grace). */
  int wait_ms = (int)(tmo + 2000);
  if (wait_ms < 0) wait_ms = -1; /* overflow → infinite */
  xEventLoopWait(ctx->loop, wait_ms);

  /* ── Build JSON result ────────────────────────────────── */
  xStrClear(ctx->result_buf);
  ctx->result_buf = xStrAppend(ctx->result_buf, "{");

  if (ctx->done && ctx->result) {
    const xCommandResult *r = ctx->result;

    ctx->result_buf =
      xStrAppendFormat(ctx->result_buf, "\"exit_code\":%d,", r->exit_code);

    /* stdout — use cJSON to produce a properly-escaped JSON string */
    if (r->stdout_buf && r->stdout_len > 0) {
      cJSON *sj  = cJSON_CreateString(r->stdout_buf);
      char  *raw = cJSON_PrintUnformatted(sj);
      ctx->result_buf =
        xStrAppendFormat(ctx->result_buf, "\"stdout\":%s,", raw);
      free(raw);
      cJSON_Delete(sj);
    } else {
      ctx->result_buf = xStrAppend(ctx->result_buf, "\"stdout\":\"\",");
    }

    /* stderr */
    if (r->stderr_buf && r->stderr_len > 0) {
      cJSON *sj  = cJSON_CreateString(r->stderr_buf);
      char  *raw = cJSON_PrintUnformatted(sj);
      ctx->result_buf =
        xStrAppendFormat(ctx->result_buf, "\"stderr\":%s,", raw);
      free(raw);
      cJSON_Delete(sj);
    } else {
      ctx->result_buf = xStrAppend(ctx->result_buf, "\"stderr\":\"\",");
    }

    ctx->result_buf = xStrAppendFormat(ctx->result_buf, "\"timed_out\":%s,",
                                       r->timed_out ? "true" : "false");
    ctx->result_buf = xStrAppendFormat(ctx->result_buf, "\"elapsed_ms\":%llu",
                                       (unsigned long long)r->elapsed_ms);
  } else {
    /* Command did not complete within the wait window */
    ctx->result_buf = xStrAppend(
      ctx->result_buf, "\"exit_code\":-1,"
                       "\"stdout\":\"\","
                       "\"stderr\":\"command timed out or failed to start\","
                       "\"timed_out\":true,"
                       "\"elapsed_ms\":0");
  }

  ctx->result_buf = xStrAppend(ctx->result_buf, "}");

  /* ── Fill output ──────────────────────────────────────── */
  memset(out, 0, sizeof(*out));
  out->type                     = xAiContentType_ToolResult;
  out->u.tool_result.id         = in->u.tool_use.id;
  out->u.tool_result.output     = ctx->result_buf; /* xStr == char* */
  out->u.tool_result.output_len = xStrLen(ctx->result_buf);

  /* ── Notify caller: command finished ────────────────────── */
  if (ctx->on_result) {
    if (ctx->done && ctx->result) {
      const xCommandResult *r = ctx->result;
      ctx->on_result(r->exit_code, r->stdout_buf ? r->stdout_len : 0,
                     r->stderr_buf ? r->stderr_len : 0, r->timed_out,
                     ctx->callback_ud);
    } else {
      ctx->on_result(-1, 0, 0, 1, ctx->callback_ud);
    }
  }

  cJSON_Delete(root);
  return xErrno_Ok;
}

/* ───────────────────── Destroy callback ───────────────────── */

static void shell_ctx_destroy(void *ud) {
  ShellCtx *ctx = (ShellCtx *)ud;
  if (!ctx) return;
  xCommandExecutorDestroy(ctx->exec);
  xStrDestroy(ctx->result_buf);
  free(ctx);
}

/* ───────────────────── Public API ───────────────────── */

XCAPI(xAiTool) xAiToolCreateShell(xEventLoop loop, const xAiShellConf *conf) {
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
  ctx->callback_ud = (conf) ? conf->callback_ud : NULL;

  ctx->result_buf = xStrCreate("");
  if (!ctx->result_buf) {
    free(ctx);
    return NULL;
  }

  ctx->exec = xCommandExecutorCreate(loop);
  if (!ctx->exec) {
    xStrDestroy(ctx->result_buf);
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
  tconf.concurrent_safe   = 0;
  tconf.needs_confirm     = 1;

  xAiTool tool = xAiToolCreate(&tconf);
  if (!tool) {
    xCommandExecutorDestroy(ctx->exec);
    xStrDestroy(ctx->result_buf);
    free(ctx);
    return NULL;
  }

  return tool;
}
