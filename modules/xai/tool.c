/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool.c - Tool registration for the xai agent core
 *
 * This TU owns the concrete layout of xAiTool. The session layer and
 * provider implementations reach the registered name / description /
 * json_schema via the ai_tool_* accessors declared in tool_private.h;
 * those helpers stay out of the installed public headers so external
 * callers never see tool internals.
 */

#include <xai/tool.h>

#include "tool_private.h"

#include <xbase/base.h>
#include <xbase/error.h>

#include <stdlib.h>
#include <string.h>

struct xAiTool_ {
  char              *name;
  char              *description; /* may be NULL            */
  char              *json_schema; /* may be NULL            */
  xAiToolHandlerFunc handler;
  void              *user_data;
  xAiToolUserDataDestroyFunc user_data_destroy; /* may be NULL */
  int                concurrent_safe;
  int                needs_confirm;
  xAiToolDoneFunc    on_done_fn;  /* may be NULL = synchronous tool   */
  void              *on_done_ud;  /* forwarded to on_done_fn          */
  xAiToolCancelFunc  on_cancel_fn; /* may be NULL = no cancel support  */
  void              *on_cancel_ud; /* forwarded to on_cancel_fn        */
};

static char *tool_strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char  *p = (char *)malloc(n);
  if (!p) return NULL;
  memcpy(p, s, n);
  return p;
}

xAiTool xAiToolCreate(const xAiToolConf *conf) {
  if (!conf || !conf->name || !conf->handler) return NULL;

  struct xAiTool_ *t = (struct xAiTool_ *)calloc(1, sizeof(*t));
  if (!t) return NULL;

  t->name            = tool_strdup(conf->name);
  t->description     = tool_strdup(conf->description);
  t->json_schema     = tool_strdup(conf->json_schema);
  t->handler           = conf->handler;
  t->user_data         = conf->user_data;
  t->user_data_destroy = conf->user_data_destroy;
  t->concurrent_safe   = conf->concurrent_safe;
  t->needs_confirm     = conf->needs_confirm;
  t->on_done_fn        = conf->on_done_fn;
  t->on_done_ud        = conf->on_done_ud;
  t->on_cancel_fn      = conf->on_cancel_fn;
  t->on_cancel_ud      = conf->on_cancel_ud;

  if (!t->name) { /* name is the only required string */
    free(t->description);
    free(t->json_schema);
    free(t);
    return NULL;
  }
  return (xAiTool)t;
}

void xAiToolDestroy(xAiTool tool) {
  struct xAiTool_ *t = (struct xAiTool_ *)tool;
  if (!t) return;
  if (t->user_data_destroy && t->user_data)
    t->user_data_destroy(t->user_data);
  free(t->name);
  free(t->description);
  free(t->json_schema);
  free(t);
}

/* ── Internal module accessors (see tool_private.h) ────────────────────── */

const char *ai_tool_name(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->name : NULL;
}

const char *ai_tool_description(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->description : NULL;
}

const char *ai_tool_json_schema(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->json_schema : NULL;
}

xErrno ai_tool_invoke(xAiTool tool, xAiQuery q, const xAiContent *in,
                      xAiContent *out) {
  struct xAiTool_ *t = (struct xAiTool_ *)tool;
  if (!t || !t->handler) return xErrno_InvalidArg;
  return t->handler(q, in, out, t->user_data);
}

int ai_tool_concurrent_safe(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->concurrent_safe : 0;
}

int ai_tool_needs_confirm(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->needs_confirm : 0;
}

xAiToolDoneFunc ai_tool_on_done_fn(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->on_done_fn : NULL;
}

void *ai_tool_on_done_ud(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->on_done_ud : NULL;
}

xAiToolCancelFunc ai_tool_on_cancel_fn(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->on_cancel_fn : NULL;
}

void *ai_tool_on_cancel_ud(xAiTool tool) {
  return tool ? ((struct xAiTool_ *)tool)->on_cancel_ud : NULL;
}
