/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_private.h - Shape of one "turn entry" message record shared
 *                  between xAgentSession history and (in Phase β) the
 *                  xAgentQuery produced-turn list.
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
 * Today (Phase α.1) only xAgentSession_::history stores these; the
 * public type names still carry the xAgentSession* prefix. Phase β
 * will wire the matching produced-list into xAgentQuery_ and may then
 * choose to rename the types to the more neutral xAgentTurnEntry. The
 * rename is deliberately deferred: mechanical-renaming in the same
 * commit as the behavioural split obscures the real churn.
 *
 * Exposed only to session.c, query.c (via session_private.h /
 * query_private.h) and their tests. Other xai translation units
 * must go through session.h / query.h.
 */

#ifndef XAGENT_TURN_PRIVATE_H
#define XAGENT_TURN_PRIVATE_H

#include <xagent/message.h> /* xAgentRole                                  */
#include <xbase/error.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Kind of a turn entry.
 *
 * History / produced lists are stored as flat arrays of "entries"
 * rather than fully composed xAgentMessages: one entry carries exactly
 * one content block. At submit time, view_build() walks the list and
 * folds consecutive Assistant-role entries into a single xAgentMessage
 * so the wire representation matches what providers expect (an
 * assistant turn is one message that may contain text, thinking and
 * tool_use blocks together).
 *
 * The tradeoff: the flat list grows longer than turn_count, but each
 * entry has a fixed, union-free shape that is trivial to own / copy
 * / free.
 */
enum xAgentSessionEntryKind_ {
  xAgentSessionEntry_Text       = 0, /**< role + text payload            */
  xAgentSessionEntry_ToolUse    = 1, /**< role==Assistant + tool_use     */
  xAgentSessionEntry_ToolResult = 2, /**< role==Tool + tool_result       */
  /**
   * role==Assistant + chain-of-thought. Payload lives in @c text /
   * @c text_len so ai_session_msg_free() stays simple. Kept as its
   * own kind so view_build() can turn it back into an
   * xAgentContentType_Thinking block on follow-up rounds — several
   * reasoning models reject tool_calls assistant messages that do
   * not echo the previous round's reasoning.
   */
  xAgentSessionEntry_Thinking   = 3,
};

/**
 * @brief One turn entry.
 *
 * All string fields are owned storage: ai_session_msg_free() is
 * responsible for releasing the populated fields based on @p kind.
 * The slot itself is caller-owned (a member of an array such as
 * xAgentSession_::history); ai_session_msg_free only releases the
 * interior and zeroes the struct.
 */
struct xAgentSessionMsg_ {
  xAgentRole                    role;
  enum xAgentSessionEntryKind_  kind;

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

  /* Wall-clock unix-ms timestamp recording when this entry was
   * created (user typed it, assistant streamed it, tool finished).
   * Zero means "unknown" — e.g. the slot was hand-rolled by a test
   * or by a callback delivering legacy on-disk data that predates
   * this field. Consumers that just want to display / sort by time
   * should fall back to "0 → now" on read. memory_jsonl uses this
   * value when writing the "ts" field and only falls back to a
   * fresh wall-clock read when the field is zero. */
  uint64_t created_at_ms;
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
void ai_session_msg_free(struct xAgentSessionMsg_ *m);

#endif /* XAGENT_TURN_PRIVATE_H */
