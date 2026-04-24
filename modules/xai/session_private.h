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
 * @brief One pending tool call captured from the provider stream.
 *
 * session.c buffers the whole set while the assistant turn is still
 * streaming (tool_use arrives as part of the stream, but dispatch
 * must wait until on_done(ToolUse) so we know the assistant message
 * is complete and every call has its full arguments string).
 */
struct xAiSessionPending_ {
  char *id;        /* tool_use_id supplied by the provider        */
  char *name;      /* tool name                                    */
  char *args_json; /* arguments JSON (owned copy)                  */
};

/**
 * @brief The session instance.
 *
 * Accessed by session.c only; the header is exposed through the
 * private tree (session_private.h) for the test suite.
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

  /* ── Assistant text accumulator for the current round ─────────── */
  char  *assist_buf;
  size_t assist_len;
  size_t assist_cap;

  /* ── Assistant reasoning / thinking accumulator (current round) ── */
  char  *reasoning_buf;
  size_t reasoning_len;
  size_t reasoning_cap;

  /* ── Pending tool calls captured during the current round ─────── */
  struct xAiSessionPending_ *pending;
  size_t                     n_pending;
  size_t                     cap_pending;

  /* ── Run-wide state ───────────────────────────────────────────── */
  int running;       /* 1 from Input accept to final on_done           */
  int cancelled;     /* xAiSessionCancel() sets this                   */
  int turn;          /* number of provider submits this run (>=1)      */

  /* Cumulative token usage across every provider round of this run.
   * Each on_provider_done folds the round's per-request numbers in;
   * finish_run hands a pointer to this struct (or NULL if no round
   * ever reported usage) to the caller's on_done. We treat -1 as
   * "unknown" on input AND on output; the accumulator keeps that
   * sentinel intact until the first field we can actually add.
   * Reset to all-(-1) at the start of every xAiSessionInput run. */
  int      saw_usage;      /* 1 once any round reported usage           */
  xAiUsage usage;          /* running totals                            */
};

#endif /* XAI_SESSION_PRIVATE_H */
