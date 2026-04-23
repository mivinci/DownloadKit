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
 * @brief One history entry owned by the session.
 *
 * Represents a single @ref xAiMessage plus all heap buffers backing
 * its content blocks. Everything inside is session-owned; the
 * session is responsible for freeing @p text / @p tool_use_* /
 * @p tool_result_* fields.
 *
 * The MVP only populates @p role + @p text (one text block per
 * message). The extra tool_* slots are reserved for Commit 4 where
 * tool_use / tool_result round-tripping lands; they stay NULL /
 * zero in the current build.
 */
struct xAiSessionMsg_ {
  xAiRole role;

  /* ── Text block (NULL = no text) ──────────────────────────────── */
  char  *text;
  size_t text_len;

  /* ── Reserved for tool_use / tool_result (MVP leaves NULL) ────── */
  char *tool_use_id;
  char *tool_use_name;
  char *tool_use_args; /* JSON object string */

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

  /* ── Rolling history (session-owned) ──────────────────────────── */
  struct xAiSessionMsg_ *history;
  size_t                 n_history;
  size_t                 cap_history;

  /* ── Assistant text accumulator for the current round ─────────── */
  char  *assist_buf;
  size_t assist_len;
  size_t assist_cap;

  /* ── In-flight run state ──────────────────────────────────────── */
  int running;   /* 1 between xAiSessionInput() accept and on_done  */
  int cancelled; /* xAiSessionCancel() sets this                    */
};

#endif /* XAI_SESSION_PRIVATE_H */
