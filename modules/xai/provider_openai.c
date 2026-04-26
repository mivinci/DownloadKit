/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * provider_openai.c - OpenAI-compatible provider implementation
 *
 * Wire format: POST <base_url>/chat/completions with
 *   { "model": ..., "messages": [...], "tools": [...],
 *     "stream": true, "temperature": ..., "max_tokens": ..., "stop": [...] }
 * over xHttpClientDoSse(). The response is a stream of SSE events
 * whose data payload is a JSON chunk:
 *   choices[0].delta.content         → text delta
 *   choices[0].delta.tool_calls[i]   → tool-call delta (id/name/args)
 *   choices[0].finish_reason         → stop | tool_calls | length | ...
 *   data: [DONE]                     → stream terminator
 *
 * We accumulate tool_call fragments per `index` and emit a single
 * ToolUse content block when the delta stream ends (either when
 * finish_reason arrives or at [DONE]).
 *
 * Concurrency:
 * - Exactly one submit() may be in flight per provider instance.
 * - cancel() merely flips a flag; the on_done chain always runs on
 *   the event loop via the SSE done callback or a 0-delay timer.
 */

#include "provider_private.h"
#include "tool_private.h"

#include <xai/message.h>
#include <xai/provider.h>
#include <xai/provider_openai.h>
#include <xai/tool.h>
#include <xbase/base.h>
#include <xbase/error.h>
#include <xbase/event.h>
#include <xbase/log.h>
#include <xbuf/buf.h>
#include <xhttp/client.h>

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── State ─────────────────────────────────────────────────────────────── */

#define XAI_OAI_MAX_TOOL_CALLS 32

struct xOaiToolCallSlot_ {
  int    used;     /* 1 if this slot has seen any delta            */
  char  *id;       /* heap-alloc'd, NUL-terminated (may be NULL)   */
  char  *name;     /* heap-alloc'd, NUL-terminated (may be NULL)   */
  char  *args_buf; /* heap-alloc'd, NUL-terminated (may be NULL)   */
  size_t args_len; /* current length of @p args_buf                */
  size_t args_cap; /* allocated capacity of @p args_buf            */
  int    emitted;  /* 1 once on_tool_call has fired for this slot  */
};

struct xOaiImpl_ {
  xEventLoop  loop;
  xHttpClient http;
  /* Config snapshots (own these). */
  char       *api_key;
  char       *base_url;      /* without trailing slash                */
  char       *organization;  /* may be NULL                           */
  char       *default_model; /* may be NULL                           */
  long        timeout_ms;

  /* ── In-flight state ── */
  int                        in_flight;
  int                        cancelled;
  xAiProviderStreamCallbacks cbs;
  void                      *cb_arg;
  xAiProviderStopReason      stop_reason; /* sticky; last write wins */
  xErrno                     stop_err;
  int                        saw_finish_reason; /* 1 once server sent one */
  struct xOaiToolCallSlot_   tool_calls[XAI_OAI_MAX_TOOL_CALLS];

  /* Token accounting for the current flight. OpenAI-compatible SSE
   * streams put a `usage` object on the chunk that carries
   * finish_reason (or, on some servers, on the [DONE] chunk). Values
   * are -1 until we actually see them; if the server never reports
   * usage (e.g. a minimal mock) we hand NULL to on_done so callers
   * can tell "unknown" from "zero". */
  int                        saw_usage;
  xAiUsage                   usage;

  xEventTimer defer_done; /* non-NULL while pending   */
};

/* ── Small string utilities (scoped to this TU) ────────────────────────── */

static char *oai_strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char  *p = (char *)malloc(n);
  if (!p) return NULL;
  memcpy(p, s, n);
  return p;
}

static void oai_free(char **p) {
  if (p && *p) {
    free(*p);
    *p = NULL;
  }
}

/* Appends (src, src_len) to the end of *dst (heap-grown, NUL-terminated). */
static int oai_str_append(char **dst, size_t *len, size_t *cap,
                          const char *src, size_t src_len) {
  if (!src || src_len == 0) return 0;
  size_t need = *len + src_len + 1; /* +1 NUL */
  if (need > *cap) {
    size_t new_cap = *cap ? *cap : 64;
    while (new_cap < need) new_cap *= 2;
    char *n = (char *)realloc(*dst, new_cap);
    if (!n) return -1;
    *dst = n;
    *cap = new_cap;
  }
  memcpy(*dst + *len, src, src_len);
  *len += src_len;
  (*dst)[*len] = '\0';
  return 0;
}

/* ── Request-body construction ─────────────────────────────────────────── */

/*
 * Map xAiRole → OpenAI role string.
 */
static const char *oai_role_str(xAiRole r) {
  switch (r) {
  case xAiRole_System:    return "system";
  case xAiRole_User:      return "user";
  case xAiRole_Assistant: return "assistant";
  case xAiRole_Tool:      return "tool";
  }
  return "user";
}

/*
 * Convert one xAiMessage into a JSON object that matches OpenAI's
 * chat.completions schema. Returns a new cJSON node or NULL on OOM.
 *
 * Shape summary:
 * - role=system/user with a single text block → {role, content: "..."}.
 * - role=user/system with mixed blocks → {role, content: "merged text"}
 *   (OpenAI supports array-of-parts too; we keep it simple for MVP).
 * - role=assistant with text only  → {role: "assistant", content: "..."}.
 * - role=assistant with tool_use   → {role: "assistant", content: null,
 *                                     tool_calls: [{id,type,function:{name,arguments}}]}.
 * - role=tool with tool_result     → {role: "tool", tool_call_id: ...,
 *                                     content: "..."}.
 */
static cJSON *oai_message_to_json(const xAiMessage *m) {
  cJSON *obj = cJSON_CreateObject();
  if (!obj) return NULL;
  cJSON_AddStringToObject(obj, "role", oai_role_str(m->role));

  if (m->role == xAiRole_Tool) {
    /* Expect one ToolResult content block. */
    const char *tool_id = "";
    const char *out     = "";
    size_t      out_len = 0;
    for (size_t i = 0; i < m->n; i++) {
      if (m->contents[i].type == xAiContentType_ToolResult) {
        tool_id = m->contents[i].u.tool_result.id
                    ? m->contents[i].u.tool_result.id : "";
        out     = m->contents[i].u.tool_result.output
                    ? m->contents[i].u.tool_result.output : "";
        out_len = m->contents[i].u.tool_result.output_len;
        break;
      }
    }
    cJSON_AddStringToObject(obj, "tool_call_id", tool_id);
    /* content is a plain string for tool role. We may have the raw
     * output with an explicit length that's not NUL-terminated, so
     * copy into a local buffer before handing it to cJSON. */
    if (out_len == 0 && out) out_len = strlen(out);
    char *tmp = (char *)malloc(out_len + 1);
    if (tmp) {
      if (out_len > 0) memcpy(tmp, out, out_len);
      tmp[out_len] = '\0';
      cJSON_AddStringToObject(obj, "content", tmp);
      free(tmp);
    } else {
      cJSON_AddStringToObject(obj, "content", "");
    }
    return obj;
  }

  if (m->role == xAiRole_Assistant) {
    /* Split text / thinking / tool_use across the content blocks.
     *
     * Why thinking: kimi-k2.6 (and other reasoning models) require
     * the client to echo back the `reasoning_content` they streamed
     * earlier — when a later round's assistant turn carries tool_calls
     * but no reasoning_content, moonshot rejects the whole request
     * with "thinking is enabled but reasoning_content is missing in
     * assistant tool call message". The session layer preserves the
     * thinking stream for us; here we serialise it alongside content
     * and tool_calls. */
    cJSON  *tool_calls     = NULL;
    xBuffer text_buf       = NULL;
    xBuffer reasoning_buf  = NULL;
    for (size_t i = 0; i < m->n; i++) {
      const xAiContent *c = &m->contents[i];
      if (c->type == xAiContentType_Text && c->u.text.text) {
        if (!text_buf) text_buf = xBufferCreate(256);
        if (text_buf) xBufferAppendStr(&text_buf, c->u.text.text);
      } else if (c->type == xAiContentType_Thinking &&
                 c->u.thinking.text) {
        if (!reasoning_buf) reasoning_buf = xBufferCreate(256);
        if (reasoning_buf) xBufferAppendStr(&reasoning_buf, c->u.thinking.text);
      } else if (c->type == xAiContentType_ToolUse) {
        if (!tool_calls) tool_calls = cJSON_CreateArray();
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddStringToObject(tc, "id",
                                c->u.tool_use.id ? c->u.tool_use.id : "");
        cJSON_AddStringToObject(tc, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name",
                                c->u.tool_use.name ? c->u.tool_use.name : "");
        cJSON_AddStringToObject(
          fn, "arguments",
          c->u.tool_use.args_json ? c->u.tool_use.args_json : "{}");
        cJSON_AddItemToObject(tc, "function", fn);
        cJSON_AddItemToArray(tool_calls, tc);
      }
    }
    if (text_buf && xBufferLen(text_buf) > 0) {
      xBufferAppend(&text_buf, "", 1); /* NUL-terminate for cJSON */
      cJSON_AddStringToObject(obj, "content", (const char *)xBufferData(text_buf));
    } else {
      cJSON_AddNullToObject(obj, "content");
    }
    if (reasoning_buf && xBufferLen(reasoning_buf) > 0) {
      xBufferAppend(&reasoning_buf, "", 1); /* NUL-terminate for cJSON */
      cJSON_AddStringToObject(obj, "reasoning_content", (const char *)xBufferData(reasoning_buf));
    }
    if (tool_calls) cJSON_AddItemToObject(obj, "tool_calls", tool_calls);
    xBufferDestroy(text_buf);
    xBufferDestroy(reasoning_buf);
    return obj;
  }

  /* system / user: concatenate all text blocks. */
  xBuffer text_buf = NULL;
  for (size_t i = 0; i < m->n; i++) {
    if (m->contents[i].type == xAiContentType_Text &&
        m->contents[i].u.text.text) {
      if (!text_buf) text_buf = xBufferCreate(256);
      if (text_buf)
        xBufferAppendStr(&text_buf, m->contents[i].u.text.text);
    }
  }
  if (text_buf && xBufferLen(text_buf) > 0) {
    xBufferAppend(&text_buf, "", 1); /* NUL-terminate for cJSON */
    cJSON_AddStringToObject(obj, "content", (const char *)xBufferData(text_buf));
  } else {
    cJSON_AddStringToObject(obj, "content", "");
  }
  xBufferDestroy(text_buf);
  return obj;
}

static cJSON *oai_tool_to_json(xAiTool tool) {
  if (!tool) return NULL;
  const char *name = ai_tool_name(tool);
  const char *desc = ai_tool_description(tool);
  const char *sch  = ai_tool_json_schema(tool);

  cJSON *obj = cJSON_CreateObject();
  if (!obj) return NULL;
  cJSON_AddStringToObject(obj, "type", "function");
  cJSON *fn = cJSON_CreateObject();
  cJSON_AddStringToObject(fn, "name", name ? name : "");
  if (desc) cJSON_AddStringToObject(fn, "description", desc);
  if (sch) {
    cJSON *parsed = cJSON_Parse(sch);
    if (parsed) {
      cJSON_AddItemToObject(fn, "parameters", parsed);
    } else {
      /* Fall back: advertise an empty object schema. */
      cJSON *empty = cJSON_CreateObject();
      cJSON_AddStringToObject(empty, "type", "object");
      cJSON_AddItemToObject(fn, "parameters", empty);
    }
  } else {
    cJSON *empty = cJSON_CreateObject();
    cJSON_AddStringToObject(empty, "type", "object");
    cJSON_AddItemToObject(fn, "parameters", empty);
  }
  cJSON_AddItemToObject(obj, "function", fn);
  return obj;
}

/*
 * Build the request body JSON as a heap-alloc'd NUL-terminated
 * string. The caller takes ownership and must free() it.
 */
static char *oai_build_body(struct xOaiImpl_            *impl,
                            const xAiProviderSubmitConf *conf) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;

  const char *model = conf->model ? conf->model : impl->default_model;
  cJSON_AddStringToObject(root, "model", model ? model : "");
  cJSON_AddBoolToObject(root, "stream", 1);

  /* Ask the server to append a final `usage` chunk with
   * prompt/completion/total tokens. OpenAI, moonshot/kimi,
   * DeepSeek and most gateways support this opt-in; servers that
   * don't recognise the field treat it as a no-op. Without this,
   * OpenAI specifically drops the usage block on streamed
   * responses. */
  {
    cJSON *so = cJSON_CreateObject();
    if (so) {
      cJSON_AddBoolToObject(so, "include_usage", 1);
      cJSON_AddItemToObject(root, "stream_options", so);
    }
  }

  if (conf->temperature >= 0.0) {
    cJSON_AddNumberToObject(root, "temperature", conf->temperature);
  }
  if (conf->max_tokens > 0) {
    cJSON_AddNumberToObject(root, "max_tokens", conf->max_tokens);
  }
  if (conf->stop) {
    cJSON *arr = cJSON_CreateArray();
    for (const char **s = conf->stop; *s; s++) {
      cJSON_AddItemToArray(arr, cJSON_CreateString(*s));
    }
    cJSON_AddItemToObject(root, "stop", arr);
  }

  cJSON *msgs = cJSON_CreateArray();
  for (size_t i = 0; i < conf->n_messages; i++) {
    cJSON *m = oai_message_to_json(&conf->messages[i]);
    if (m) cJSON_AddItemToArray(msgs, m);
  }
  cJSON_AddItemToObject(root, "messages", msgs);

  if (conf->tools && conf->n_tools > 0) {
    cJSON *tools = cJSON_CreateArray();
    for (size_t i = 0; i < conf->n_tools; i++) {
      /* SubmitConf.tools is `const xAiTool **` — an array of handle
       * pointers. Deref once to reach the handle itself. The handle
       * is opaque (void*), so the const here is advisory only (the
       * accessors don't mutate it). */
      if (!conf->tools[i]) continue;
      cJSON *t = oai_tool_to_json(*conf->tools[i]);
      if (t) cJSON_AddItemToArray(tools, t);
    }
    cJSON_AddItemToObject(root, "tools", tools);
  }

  char *out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

/* ── Flight-state helpers ──────────────────────────────────────────────── */

static void oai_tool_calls_reset(struct xOaiImpl_ *impl) {
  for (int i = 0; i < XAI_OAI_MAX_TOOL_CALLS; i++) {
    struct xOaiToolCallSlot_ *s = &impl->tool_calls[i];
    oai_free(&s->id);
    oai_free(&s->name);
    oai_free(&s->args_buf);
    s->args_len = 0;
    s->args_cap = 0;
    s->used     = 0;
    s->emitted  = 0;
  }
}

/*
 * Emit any tool_calls we have assembled but not yet surfaced.
 * The tool_use content block borrows impl-owned buffers, so the
 * emit happens before we tear down the flight state.
 */
static void oai_emit_tool_calls(struct xOaiImpl_ *impl) {
  if (!impl->cbs.on_tool_call) return;
  for (int i = 0; i < XAI_OAI_MAX_TOOL_CALLS; i++) {
    struct xOaiToolCallSlot_ *s = &impl->tool_calls[i];
    if (!s->used || s->emitted) continue;

    xAiContent c = {0};
    c.type                 = xAiContentType_ToolUse;
    c.u.tool_use.id        = s->id   ? s->id   : "";
    c.u.tool_use.name      = s->name ? s->name : "";
    c.u.tool_use.args_json =
      (s->args_buf && s->args_len > 0) ? s->args_buf : "{}";
    impl->cbs.on_tool_call(&c, impl->cb_arg);
    s->emitted = 1;
  }
}

/*
 * Finalise the flight: emit any pending tool_calls, fire on_done
 * with the sticky (reason, err), and reset state so the provider
 * accepts another submit().
 */
static void oai_finish_flight(struct xOaiImpl_ *impl) {
  if (!impl->in_flight) return;
  oai_emit_tool_calls(impl);

  xAiProviderDoneFunc   done   = impl->cbs.on_done;
  xAiProviderStopReason reason = impl->stop_reason;
  xErrno                err    = impl->stop_err;
  void                 *arg    = impl->cb_arg;

  /* Snapshot usage before we reset state. We pass a pointer through
   * the callback, so it must outlive the rest of this function —
   * using a local here is fine since on_done is invoked synchronously
   * just below and the pointer is only valid for that call. */
  xAiUsage usage_snapshot = impl->usage;
  int      had_usage      = impl->saw_usage;

  /* Clear flight state BEFORE firing on_done, so a callback that
   * synchronously issues another submit() sees a clean provider. */
  impl->in_flight   = 0;
  impl->cancelled   = 0;
  impl->defer_done  = NULL;
  impl->cbs         = (xAiProviderStreamCallbacks){0};
  impl->cb_arg      = NULL;
  impl->stop_reason = xAiProviderStop_EndTurn;
  impl->stop_err    = xErrno_Ok;
  impl->saw_finish_reason = 0;
  impl->saw_usage   = 0;
  impl->usage.prompt_tokens     = -1;
  impl->usage.completion_tokens = -1;
  impl->usage.total_tokens      = -1;
  oai_tool_calls_reset(impl);

  if (done) done(reason, err, had_usage ? &usage_snapshot : NULL, arg);
}

/* ── SSE delta parsing ─────────────────────────────────────────────────── */

/*
 * Parse a "finish_reason" string into our provider-level stop enum.
 */
static xAiProviderStopReason oai_parse_finish_reason(const char *fr) {
  if (!fr) return xAiProviderStop_EndTurn;
  if (strcmp(fr, "stop") == 0)           return xAiProviderStop_EndTurn;
  if (strcmp(fr, "tool_calls") == 0)     return xAiProviderStop_ToolUse;
  if (strcmp(fr, "length") == 0)         return xAiProviderStop_MaxTokens;
  if (strcmp(fr, "content_filter") == 0) return xAiProviderStop_Error;
  /* "function_call" is the legacy name for tool_calls. */
  if (strcmp(fr, "function_call") == 0)  return xAiProviderStop_ToolUse;
  return xAiProviderStop_EndTurn;
}

/*
 * Handle one SSE event whose data is the JSON chunk body.
 * Returns non-zero to tell xHttpClientDoSse to close the stream.
 */
static int oai_handle_chunk(struct xOaiImpl_ *impl, const char *data) {
  if (!data) return 0;

  /* Per-chunk dump is very noisy (one line per SSE delta — ~50+ per
   * turn for thinking models) and actively fights the REPL's stdout
   * stream: without a stderr→stdout sync, each assistant char on
   * stdout gets pushed to line-start/-end by the next stderr line.
   * Keep it at L3 for targeted debugging; the L1 POST body dump
   * above is enough for normal tracing. */
  XDEBUGL3("[xai/openai] chunk: %s", data);

  /* Per OpenAI spec, the terminating event carries the literal
   * string "[DONE]" (no JSON). Anything else is parseable JSON. */
  if (strcmp(data, "[DONE]") == 0) {
    return 1; /* close the stream; on_done callback follows */
  }

  cJSON *root = cJSON_Parse(data);
  if (!root) {
    XDEBUG("[xai/openai] failed to parse SSE chunk");
    return 0;
  }

  cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
  cJSON *choice  = choices ? cJSON_GetArrayItem(choices, 0) : NULL;

  /* OpenAI-compatible servers carry token accounting in a top-level
   * `usage` object, NOT under `choices[].delta`. It typically arrives
   * on either (a) the chunk that also carries the final
   * finish_reason, or (b) a standalone chunk right before
   * [DONE]. Some gateways send `"choices":[]` with usage only — so
   * we MUST parse usage before bailing on a missing choice[0]. We
   * keep "last value wins" semantics (any later chunk's usage
   * overrides earlier partial numbers, though in practice usage
   * only appears once). */
  cJSON *usage_obj = cJSON_GetObjectItemCaseSensitive(root, "usage");
  if (cJSON_IsObject(usage_obj)) {
    cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage_obj, "prompt_tokens");
    cJSON *ct = cJSON_GetObjectItemCaseSensitive(usage_obj,
                                                 "completion_tokens");
    cJSON *tt = cJSON_GetObjectItemCaseSensitive(usage_obj, "total_tokens");
    if (cJSON_IsNumber(pt)) impl->usage.prompt_tokens     = pt->valueint;
    if (cJSON_IsNumber(ct)) impl->usage.completion_tokens = ct->valueint;
    if (cJSON_IsNumber(tt)) impl->usage.total_tokens      = tt->valueint;
    impl->saw_usage = 1;
  }

  if (!choice) {
    cJSON_Delete(root);
    return 0;
  }

  cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
  if (delta) {
    cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
    if (cJSON_IsString(content) && content->valuestring &&
        impl->cbs.on_text) {
      const char *s = content->valuestring;
      impl->cbs.on_text(s, strlen(s), impl->cb_arg);
    }

    /* `reasoning_content` is kimi/DeepSeek/o1's chain-of-thought
     * stream. We forward it verbatim; the session layer decides
     * whether to store it (for round-2 echo-back) and/or surface it
     * to the caller. */
    cJSON *reasoning =
      cJSON_GetObjectItemCaseSensitive(delta, "reasoning_content");
    if (cJSON_IsString(reasoning) && reasoning->valuestring &&
        impl->cbs.on_thinking) {
      const char *s = reasoning->valuestring;
      impl->cbs.on_thinking(s, strlen(s), impl->cb_arg);
    }

    cJSON *tool_calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
    if (cJSON_IsArray(tool_calls)) {
      cJSON *tc = NULL;
      cJSON_ArrayForEach(tc, tool_calls) {
        cJSON *idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
        int    i   = cJSON_IsNumber(idx) ? idx->valueint : 0;
        if (i < 0 || i >= XAI_OAI_MAX_TOOL_CALLS) continue;

        struct xOaiToolCallSlot_ *slot = &impl->tool_calls[i];
        slot->used = 1;

        cJSON *id = cJSON_GetObjectItemCaseSensitive(tc, "id");
        if (cJSON_IsString(id) && id->valuestring && !slot->id) {
          slot->id = oai_strdup(id->valuestring);
        }

        cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
        if (fn) {
          cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
          if (cJSON_IsString(name) && name->valuestring && !slot->name) {
            slot->name = oai_strdup(name->valuestring);
          }
          cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
          if (cJSON_IsString(args) && args->valuestring) {
            const char *s = args->valuestring;
            oai_str_append(&slot->args_buf, &slot->args_len, &slot->args_cap,
                           s, strlen(s));
          }
        }
      }
    }
  }

  cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
  if (cJSON_IsString(finish) && finish->valuestring) {
    impl->stop_reason = oai_parse_finish_reason(finish->valuestring);
    impl->stop_err =
      (impl->stop_reason == xAiProviderStop_Error) ? xErrno_SysError
                                                   : xErrno_Ok;
    impl->saw_finish_reason = 1;
  }

  cJSON_Delete(root);
  return 0;
}

/* ── SSE callbacks ─────────────────────────────────────────────────────── */

static int oai_on_sse_event(const xSseEvent *ev, void *arg) {
  struct xOaiImpl_ *impl = (struct xOaiImpl_ *)arg;
  if (impl->cancelled) return 1; /* close stream immediately */
  if (!ev || !ev->data) return 0;
  return oai_handle_chunk(impl, ev->data);
}

static void oai_on_sse_done(int curl_code, void *arg) {
  struct xOaiImpl_ *impl = (struct xOaiImpl_ *)arg;
  if (!impl->in_flight) return; /* destroyed mid-flight */

  if (impl->cancelled) {
    impl->stop_reason = xAiProviderStop_Cancelled;
    impl->stop_err    = xErrno_Ok;
  } else if (!impl->saw_finish_reason && curl_code != 0) {
    /* Transport failed and the server never sent an explicit
     * finish_reason — surface as an error. Note: a clean server
     * close after "data: [DONE]" may still report non-zero curl
     * codes depending on libcurl version, so we gate on whether
     * we actually observed a finish_reason, not just curl_code. */
    impl->stop_reason = xAiProviderStop_Error;
    impl->stop_err    = xErrno_SysError;
  }
  /* Otherwise: keep whatever stop_reason the last finish_reason
   * set (or the default EndTurn if the stream ended cleanly
   * without one). */

  oai_finish_flight(impl);
}

/* ── Vtable ops ────────────────────────────────────────────────────────── */

static xErrno oai_submit(void                             *impl_p,
                         const xAiProviderSubmitConf      *conf,
                         const xAiProviderStreamCallbacks *cbs,
                         void                             *cb_arg) {
  struct xOaiImpl_ *impl = (struct xOaiImpl_ *)impl_p;
  if (!impl || !conf || !cbs) return xErrno_InvalidArg;
  if (impl->in_flight) return xErrno_InvalidState;
  if (!impl->api_key) return xErrno_InvalidState;

  /* Reset per-flight state. */
  impl->in_flight   = 1;
  impl->cancelled   = 0;
  impl->cbs         = *cbs;
  impl->cb_arg      = cb_arg;
  impl->stop_reason = xAiProviderStop_EndTurn;
  impl->stop_err    = xErrno_Ok;
  impl->saw_finish_reason = 0;
  impl->saw_usage   = 0;
  impl->usage.prompt_tokens     = -1;
  impl->usage.completion_tokens = -1;
  impl->usage.total_tokens      = -1;
  oai_tool_calls_reset(impl);

  /* Compose URL: <base>/chat/completions */
  const char *base = impl->base_url ? impl->base_url
                                    : "https://api.openai.com/v1";
  size_t      base_len = strlen(base);
  const char *tail     = "/chat/completions";
  size_t      tail_len = strlen(tail);
  char       *url      = (char *)malloc(base_len + tail_len + 1);
  if (!url) {
    impl->in_flight = 0;
    return xErrno_NoMemory;
  }
  memcpy(url, base, base_len);
  memcpy(url + base_len, tail, tail_len);
  url[base_len + tail_len] = '\0';

  /* Body. */
  char *body = oai_build_body(impl, conf);
  if (!body) {
    free(url);
    impl->in_flight = 0;
    return xErrno_NoMemory;
  }

  /* Debug: echo the request body. Invaluable when the server goes
   * quiet on the follow-up round of a tool loop (reply_bytes=0 but
   * no explicit error), but verbose enough that it visually competes
   * with stdout in interactive REPLs — keep at L3 together with the
   * per-chunk dump. Flip XK_DEBUG_LEVEL to 3 when you need wire-level
   * tracing. */
  XDEBUGL3("[xai/openai] POST %s body=%s", url, body);

  /* Headers: we must hold the storage until xHttpClientDoSse returns. */
  char auth_buf[512];
  snprintf(auth_buf, sizeof(auth_buf), "Authorization: Bearer %s",
           impl->api_key);
  char org_buf[256];
  org_buf[0] = '\0';
  if (impl->organization) {
    snprintf(org_buf, sizeof(org_buf), "OpenAI-Organization: %s",
             impl->organization);
  }

  const char *hdrs[5];
  size_t      nhdr = 0;
  hdrs[nhdr++] = auth_buf;
  hdrs[nhdr++] = "Content-Type: application/json";
  if (org_buf[0]) hdrs[nhdr++] = org_buf;
  hdrs[nhdr++] = NULL;

  xHttpRequestConf req = {0};
  req.url        = url;
  req.method     = xHttpMethod_POST;
  req.body       = body;
  req.body_len   = strlen(body);
  req.headers    = hdrs;
  req.timeout_ms = impl->timeout_ms;

  xErrno err = xHttpClientDoSse(impl->http, &req, oai_on_sse_event,
                                oai_on_sse_done, impl);

  /* xHttpClientDoSse copies the body + headers internally (see
   * xhttp/sse.c), so we can free our scratch buffers now. */
  free(url);
  free(body);

  if (err != xErrno_Ok) {
    impl->in_flight = 0;
    impl->cbs       = (xAiProviderStreamCallbacks){0};
    impl->cb_arg    = NULL;
  }
  return err;
}

static void oai_cancel(void *impl_p) {
  struct xOaiImpl_ *impl = (struct xOaiImpl_ *)impl_p;
  if (!impl || !impl->in_flight || impl->cancelled) return;
  impl->cancelled = 1;
  /* The next SSE event (or the upstream done callback) will observe
   * `cancelled` and close the stream; on_done flows through
   * oai_on_sse_done. If the flight is already stuck (no events
   * pending), the SSE stream will still get torn down by curl the
   * next time its timer ticks. */
}

static void oai_destroy(void *impl_p) {
  struct xOaiImpl_ *impl = (struct xOaiImpl_ *)impl_p;
  if (!impl) return;
  /* Session layer guarantees no flight is in progress by the time
   * we reach here — it cancels and waits for on_done. We still
   * defensively cancel any pending defer timer. */
  if (impl->defer_done) {
    xEventLoopTimerCancel(impl->loop, impl->defer_done);
    impl->defer_done = NULL;
  }
  oai_tool_calls_reset(impl);
  oai_free(&impl->api_key);
  oai_free(&impl->base_url);
  oai_free(&impl->organization);
  oai_free(&impl->default_model);
  free(impl);
}

static const xAiProviderVtable kOpenAIVtable = {
  .submit  = oai_submit,
  .cancel  = oai_cancel,
  .destroy = oai_destroy,
};

/* ── Public constructor ────────────────────────────────────────────────── */

xAiProvider xAiProviderOpenAICreate(xEventLoop           loop,
                                    xHttpClient          http,
                                    const xAiOpenAIConf *conf) {
  if (!loop || !http || !conf || !conf->api_key) return NULL;

  struct xOaiImpl_ *impl = (struct xOaiImpl_ *)calloc(1, sizeof(*impl));
  if (!impl) return NULL;

  impl->loop          = loop;
  impl->http          = http;
  impl->api_key       = oai_strdup(conf->api_key);
  impl->base_url      = oai_strdup(conf->base_url);
  impl->organization  = oai_strdup(conf->organization);
  impl->default_model = oai_strdup(conf->default_model);
  impl->timeout_ms    = conf->timeout_ms;
  impl->stop_reason   = xAiProviderStop_EndTurn;

  if (!impl->api_key) {
    oai_destroy(impl);
    return NULL;
  }

  struct xAiProvider_ *base =
    (struct xAiProvider_ *)calloc(1, sizeof(*base));
  if (!base) {
    oai_destroy(impl);
    return NULL;
  }
  base->vt  = &kOpenAIVtable;
  base->ctx = impl;
  return (xAiProvider)base;
}
