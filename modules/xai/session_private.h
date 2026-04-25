/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * session_private.h - Internal layout of xAiSession_ and its
 *                     cross-TU helpers shared with query.c
 *
 * Exposed only to session.c, query.c and their tests. Other xai
 * translation units must go through session.h.
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
   * @c text_len so ai_session_msg_free() stays simple. Kept as its
   * own kind so view_build() can turn it back into an
   * xAiContentType_Thinking block on follow-up rounds — several
   * reasoning models reject tool_calls assistant messages that do
   * not echo the previous round's reasoning.
   */
  xAiSessionEntry_Thinking   = 3,
};

/**
 * @brief One history entry owned by the session.
 *
 * Everything inside is session-owned; ai_session_msg_free() is
 * responsible for releasing the populated fields based on @p kind.
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
 * Accessed by session.c and query.c (through the back-pointer on
 * xAiQuery_). Session holds everything durable across a conversation:
 * agent reference, caller-facing callbacks, resolved configuration,
 * and the rolling history. Everything that belongs to "one in-flight
 * run of the tool loop" lives on the embedded Query (@c query) —
 * see query_private.h.
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

/* Fallback cap if neither the caller nor the agent set max_turns.
 * Placed in the shared header so query.c can honour it too. */
#define XAI_SESSION_DEFAULT_MAX_TURNS 16

/* ── Cross-TU helpers (session.c implementers, query.c consumers) ── */

/**
 * @brief Release the fields of a history entry in place.
 *
 * The slot itself is caller-owned storage (a member of @c session->history);
 * this only frees the owned strings and zeroes the struct.
 */
void ai_session_msg_free(struct xAiSessionMsg_ *m);

/**
 * @brief Append a (role, text) history entry. @p text is duplicated.
 */
xErrno ai_history_append_text(struct xAiSession_ *s, xAiRole role,
                              const char *text, size_t len);

/**
 * @brief Append an Assistant chain-of-thought entry. @p text is duplicated.
 */
xErrno ai_history_append_thinking(struct xAiSession_ *s, const char *text,
                                  size_t len);

/**
 * @brief Append an Assistant tool_use entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_use(struct xAiSession_ *s, const char *id,
                                  const char *name, const char *args);

/**
 * @brief Append a Tool tool_result entry. Every string is duplicated.
 */
xErrno ai_history_append_tool_result(struct xAiSession_ *s, const char *id,
                                     const char *output, size_t output_len,
                                     int is_error);

/**
 * @brief Finish the current run: reset the Query and fire on_done.
 *
 * Snapshots the usage accumulator, clears the Query's round buffers
 * and run flags, then invokes the caller's on_done callback with
 * @p reason (and the snapshot if any round reported usage).
 */
void ai_session_finish_run(struct xAiSession_ *s, xAiDoneReason reason);

#endif /* XAI_SESSION_PRIVATE_H */
