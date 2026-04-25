/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_private.h - Shape of one "turn entry" message record shared
 *                  between xAiSession history and (in Phase β) the
 *                  xAiQuery produced-turn list.
 *
 * Background
 * ----------
 * Both Session and Query need to persist the same kind of thing:
 * a flat list of atomic message records, each carrying exactly one
 * content block (user text, assistant text, assistant thinking,
 * assistant tool_use, tool role tool_result). Keeping the record
 * shape in its own header lets Session own the durable conversation
 * history while Query owns the transient per-run produced list —
 * both point into records of identical layout, so view_build() can
 * walk an input+produced sequence without caring which side owns
 * which entry.
 *
 * Today (Phase α.1) only xAiSession_::history stores these; the
 * public type names still carry the xAiSession* prefix. Phase β
 * will wire the matching produced-list into xAiQuery_ and may then
 * choose to rename the types to the more neutral xAiTurnEntry. The
 * rename is deliberately deferred: mechanical-renaming in the same
 * commit as the behavioural split obscures the real churn.
 *
 * Exposed only to session.c, query.c (via session_private.h /
 * query_private.h) and their tests. Other xai translation units
 * must go through session.h / query.h.
 */

#ifndef XAI_TURN_PRIVATE_H
#define XAI_TURN_PRIVATE_H

#include <xai/message.h> /* xAiRole                                  */
#include <xbase/error.h>

#include <stddef.h>

/**
 * @brief Kind of a turn entry.
 *
 * History / produced lists are stored as flat arrays of "entries"
 * rather than fully composed xAiMessages: one entry carries exactly
 * one content block. At submit time, view_build() walks the list and
 * folds consecutive Assistant-role entries into a single xAiMessage
 * so the wire representation matches what providers expect (an
 * assistant turn is one message that may contain text, thinking and
 * tool_use blocks together).
 *
 * The tradeoff: the flat list grows longer than turn_count, but each
 * entry has a fixed, union-free shape that is trivial to own / copy
 * / free.
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
 * @brief One turn entry.
 *
 * All string fields are owned storage: ai_session_msg_free() is
 * responsible for releasing the populated fields based on @p kind.
 * The slot itself is caller-owned (a member of an array such as
 * xAiSession_::history); ai_session_msg_free only releases the
 * interior and zeroes the struct.
 */
struct xAiSessionMsg_ {
  xAiRole                    role;
  enum xAiSessionEntryKind_  kind;

  /* kind == Text / Thinking */
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
 * @brief Release the fields of a turn entry in place.
 *
 * The slot itself is caller-owned storage; this only frees the owned
 * strings and zeroes the struct. Name is kept as @c ai_session_msg_free
 * in Phase α.1 so existing call sites stay untouched; Phase β may
 * rename to @c ai_turn_entry_free once Query starts storing these
 * records too.
 */
void ai_session_msg_free(struct xAiSessionMsg_ *m);

#endif /* XAI_TURN_PRIVATE_H */
