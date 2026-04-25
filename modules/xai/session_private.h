/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session_private.h - Internal layout of xAiSession_
 *
 * Exposed only to session.c and its tests. Other xai translation
 * units must go through session.h.
 */

#ifndef XAI_SESSION_PRIVATE_H
#define XAI_SESSION_PRIVATE_H

#include <xai/session.h>

#include "query_private.h"

#include <stddef.h>

/**
 * @brief Kind of a history entry.
 *
 * History is stored as a flat list of "entries" rather than fully
 * composed xAiMessages: one entry carries exactly one content block.
 * At submit time, view_build() walks the list and folds consecutive
 * Assistant-role entries into a single xAiMessage so the wire
 * representation matches what providers expect (an assistant turn is
 * one message that may contain text and tool_use blocks together).
 *
 * The tradeoff: history grows longer than turn_count, but each entry
 * has a fixed, union-free shape that is trivial to own/copy/free.
 */
enum xAiSessionEntryKind_ {
  xAiSessionEntry_Text       = 0, /**< role + text payload            */
  xAiSessionEntry_ToolUse    = 1, /**< role==Assistant + tool_use     */
  xAiSessionEntry_ToolResult = 2, /**< role==Tool + tool_result       */
  /**
   * role==Assistant + chain-of-thought. Payload lives in @c text /
   * @c text_len so msg_free() stays simple. Kept as its own kind so
   * view_build() can turn it back into an xAiContentType_Thinking
   * block on follow-up rounds — several reasoning models reject
   * tool_calls assistant messages that do not echo the previous
   * round's reasoning.
   */
  xAiSessionEntry_Thinking   = 3,
};

/**
 * @brief One history entry owned by the session.
 *
 * Everything inside is session-owned; msg_free() is responsible for
 * releasing the populated fields based on @p kind.
 */
struct xAiSessionMsg_ {
  xAiRole                    role;
  enum xAiSessionEntryKind_  kind;

  /* kind == Text */
  char  *text;
  size_t text_len;

  /* kind == ToolUse */
  char *tool_use_id;
  char *tool_use_name;
  char *tool_use_args; /* JSON object string */

  /* kind == ToolResult */
  char  *tool_result_id;
  char  *tool_result_output;
  size_t tool_result_output_len;
  int    tool_result_is_error;
};

/**
 * @brief The session instance.
 *
 * Accessed by session.c only; the header is exposed through the
 * private tree (session_private.h) for the test suite.
 *
 * Session holds everything that is durable across the lifetime of a
 * conversation: agent reference, caller-facing callbacks, resolved
 * configuration, and the rolling history. Everything that belongs to
 * "one in-flight run of the tool loop" lives on the embedded Query
 * (@c query) — see query_private.h.
 */
struct xAiSession_ {
  xAiAgent            agent; /* borrowed                              */
  xAiSessionCallbacks cbs;   /* copied by value                       */

  /* ── Configuration snapshot (borrowed from conf or agent) ─────── */
  const char *system_prompt; /* borrowed from conf or agent           */
  const char *model;         /* borrowed from conf or agent           */
  int         max_turns;     /* resolved (>0) or 0 = unlimited        */
  int         max_tokens;    /* resolved per-round cap                */
  size_t      context_budget;

  /* ── Rolling history (session-owned, flat entries) ────────────── */
  struct xAiSessionMsg_ *history;
  size_t                 n_history;
  size_t                 cap_history;

  /* ── In-flight run state ──────────────────────────────────────── */
  /* One Query at a time for now. Embedded, not heap-allocated: the
   * struct is small and we never observe more than one concurrent
   * run per Session. Promotion to a pointer (or a list) is a
   * follow-up when SystemSynthesized queries start coexisting with
   * user-initiated ones — see docs/todo/xai_architecture.md §8. */
  struct xAiQuery_ query;
};

#endif /* XAI_SESSION_PRIVATE_H */
