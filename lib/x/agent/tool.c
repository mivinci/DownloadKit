/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tool.c - Tool registration for the xai agent core
 *
 * This TU owns the concrete layout of xAgentTool. The session layer and
 * provider implementations reach the registered name / description /
 * json_schema via the ai_tool_* accessors declared in tool_private.h;
 * those helpers stay out of the installed public headers so external
 * callers never see tool internals.
 */

#include <xagent/tool.h>

#include "tool_private.h"

#include <xbase/base.h>
#include <xbase/error.h>

#include <stdlib.h>
#include <string.h>

struct xAgentTool_ {
  char              *name;
  char              *description; /* may be NULL            */
  char              *json_schema; /* may be NULL            */
  xAgentToolHandlerFunc handler;
  void              *user_data;
  xAgentToolUserDataDestroyFunc user_data_destroy; /* may be NULL */
  int                concurrent_safe;
  int                needs_confirm;
  xAgentToolDoneFunc    on_done_fn;  /* may be NULL = synchronous tool   */
  void              *on_done_ud;  /* forwarded to on_done_fn          */
  xAgentToolCancelFunc  on_cancel_fn; /* may be NULL = no cancel support  */
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

xAgentTool xAgentToolCreate(const xAgentToolConf *conf) {
  if (!conf || !conf->name || !conf->handler) return NULL;

  struct xAgentTool_ *t = (struct xAgentTool_ *)calloc(1, sizeof(*t));
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
  return (xAgentTool)t;
}

void xAgentToolDestroy(xAgentTool tool) {
  struct xAgentTool_ *t = (struct xAgentTool_ *)tool;
  if (!t) return;
  if (t->user_data_destroy && t->user_data)
    t->user_data_destroy(t->user_data);
  free(t->name);
  free(t->description);
  free(t->json_schema);
  free(t);
}

void *xAgentToolUserData(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->user_data : NULL;
}

/* ── Internal module accessors (see tool_private.h) ────────────────────── */

const char *ai_tool_name(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->name : NULL;
}

const char *ai_tool_description(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->description : NULL;
}

const char *ai_tool_json_schema(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->json_schema : NULL;
}

xErrno ai_tool_invoke(xAgentTool tool, xAgentQuery q, const xAgentContent *in,
                      xAgentContent *out) {
  struct xAgentTool_ *t = (struct xAgentTool_ *)tool;
  if (!t || !t->handler) return xErrno_InvalidArg;
  return t->handler(q, in, out, t->user_data);
}

int ai_tool_concurrent_safe(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->concurrent_safe : 0;
}

int ai_tool_needs_confirm(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->needs_confirm : 0;
}

xAgentToolDoneFunc ai_tool_on_done_fn(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->on_done_fn : NULL;
}

void *ai_tool_on_done_ud(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->on_done_ud : NULL;
}

xAgentToolCancelFunc ai_tool_on_cancel_fn(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->on_cancel_fn : NULL;
}

void *ai_tool_on_cancel_ud(xAgentTool tool) {
  return tool ? ((struct xAgentTool_ *)tool)->on_cancel_ud : NULL;
}
