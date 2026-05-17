/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * message.h - Conversation message model for the xai agent core
 *
 * Defines the common data structures used across xai/tool, xai/provider
 * and xai/session: roles, content blocks (tagged union of text / tool
 * use / tool result), and messages (a role plus an ordered list of
 * content blocks). These types are the lingua franca between the
 * caller, the provider, and registered tools.
 *
 * All string pointers stored in these structures are borrowed by
 * default: the library never takes ownership. Helpers below (e.g.
 * xAgentMessageFromText) construct shallow views; callers that need
 * longer-lived copies must make their own.
 */

#ifndef XAGENT_MESSAGE_H
#define XAGENT_MESSAGE_H

#include <stddef.h>
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Speaker role of a message.
 *
 * Mirrors the OpenAI / Anthropic chat role vocabulary. Zero value
 * (xAgentRole_System) is chosen so that a zero-initialised xAgentMessage
 * defaults to a system-role empty message.
 */
XDEF_ENUM(xAgentRole){
  xAgentRole_System    = 0, /**< System / developer instructions            */
  xAgentRole_User      = 1, /**< End-user input                              */
  xAgentRole_Assistant = 2, /**< Model output                                */
  xAgentRole_Tool      = 3, /**< Tool result fed back to the model           */
  xAgentRole_Summary   = 4, /**< Compressed context from prior history       */
};

/**
 * @brief Content block kind carried by a message.
 */
XDEF_ENUM(xAgentContentType){
  xAgentContentType_Text       = 0, /**< Plain text block                    */
  xAgentContentType_ToolUse    = 1, /**< Model-issued tool invocation        */
  xAgentContentType_ToolResult = 2, /**< Tool execution result               */
  xAgentContentType_Thinking   = 3, /**< Model chain-of-thought (echo-back)  */
};

/**
 * @brief A single content block inside a message.
 *
 * Tagged union: the @p type field selects which member of @p u is
 * active. All pointer fields are borrowed; the library never owns the
 * backing memory. Strings are NUL-terminated unless an explicit length
 * field is provided alongside.
 */
XDEF_STRUCT(xAgentContent) {
  xAgentContentType type;
  union {
    /** Active when type == xAgentContentType_Text. */
    struct {
      const char *text; /**< UTF-8 text (must not be NULL)                */
      size_t      len;  /**< Byte length of @p text                       */
    } text;
    /** Active when type == xAgentContentType_ToolUse. */
    struct {
      const char *id;        /**< Call id assigned by the provider        */
      const char *name;      /**< Tool name being invoked                 */
      const char *args_json; /**< Arguments as a JSON object (may be "{}")*/
    } tool_use;
    /** Active when type == xAgentContentType_ToolResult. */
    struct {
      const char *id;         /**< Matching tool_use id                   */
      const char *output;     /**< Result payload (text or JSON)          */
      size_t      output_len; /**< Byte length of @p output               */
      int         is_error;   /**< Non-zero if the tool reported failure  */
    } tool_result;
    /** Active when type == xAgentContentType_Thinking.
     *
     * Carries the model's streamed chain-of-thought ("reasoning_content"
     * on kimi/DeepSeek-R1/o1, "thinking" blocks on Anthropic). The
     * session layer echoes these back inside the assistant turn on
     * follow-up rounds; several thinking-capable models reject the
     * request otherwise (e.g. kimi-k2.6 returns
     * "thinking is enabled but reasoning_content is missing in
     *  assistant tool call message"). */
    struct {
      const char *text; /**< UTF-8 reasoning text (must not be NULL)   */
      size_t      len;  /**< Byte length of @p text                    */
    } thinking;
  } u;
};

/**
 * @brief A message: one role plus an ordered list of content blocks.
 *
 * Zero-initialised xAgentMessage is a valid empty system message. The
 * @p contents pointer is borrowed; the library never frees it.
 */
XDEF_STRUCT(xAgentMessage) {
  xAgentRole           role;     /**< Speaker role                           */
  const xAgentContent *contents; /**< Array of @p n content blocks, or NULL  */
  size_t               n;        /**< Number of content blocks               */
};

/* ── Convenience builders (borrow-only, no allocation) ─────────────── */

/**
 * @brief Build a single text content block from a NUL-terminated string.
 *
 * The returned struct borrows @p text; the caller must keep @p text
 * alive for as long as the returned content is used.
 *
 * @param text  NUL-terminated UTF-8 string (must not be NULL).
 * @return      A text content block (type = xAgentContentType_Text).
 */
XCAPI(xAgentContent) xAgentContentText(const char *text);

/**
 * @brief Build a user-role message containing a single text block.
 *
 * The returned message borrows @p text via an internal, caller-owned
 * xAgentContent. Typical usage:
 *
 * @code
 *   xAgentContent c = xAgentContentText("hello");
 *   xAgentMessage m = xAgentMessageFromContent(xAgentRole_User, &c, 1);
 *   xAgentSessionInput(sess, m);
 * @endcode
 *
 * For the very common "send one user string" case, @ref
 * xAgentMessageFromText wraps this pattern using a thread-local storage
 * slot so the caller does not have to manage the xAgentContent lifetime.
 *
 * @param role     Speaker role.
 * @param contents Array of content blocks (may be NULL iff @p n == 0).
 * @param n        Number of content blocks.
 * @return         A message view.
 */
XCAPI(xAgentMessage) xAgentMessageFromContent(xAgentRole role, const xAgentContent *contents,
                                              size_t n);

/**
 * @brief Build a user-role message carrying a single text block.
 *
 * Convenience for `xAgentSessionInput(sess, xAgentMessageFromText("..."))`.
 * The returned message references an internal per-thread buffer; the
 * view is valid until the next call to xAgentMessageFromText() on the
 * same thread. Do not store the returned value across calls.
 *
 * @param text  NUL-terminated UTF-8 string (must not be NULL).
 * @return      A user-role message with one text block.
 */
XCAPI(xAgentMessage) xAgentMessageFromText(const char *text);

/* ── Token accounting ─────────────────────────────────────────────── */

/**
 * @brief Token usage reported by the model for one (or more) rounds.
 *
 * Providers that don't report a particular field use @c -1 as a
 * "not available" sentinel; clients should check for this before
 * displaying or summing. The public convention across the xai stack
 * is to initialise with all three fields set to @c -1 so that
 * callers can trust "unknown" vs "zero".
 *
 * The session layer accumulates usage across tool-loop rounds: each
 * on_done() carries the running sum for the entire xAgentSessionInput()
 * call, not just the last round. Providers report per-request
 * numbers — provider_openai extracts them from the SSE chunk that
 * carries the final `usage` object (OpenAI) or the equivalent.
 */
XDEF_STRUCT(xAgentUsage) {
  int prompt_tokens;     /**< Input/prompt side; -1 if unknown       */
  int completion_tokens; /**< Output/completion side; -1 if unknown  */
  int total_tokens;      /**< Sum (may be > prompt+completion on
                              providers that count reasoning); -1 if
                              unknown                                 */
};

#endif /* XAGENT_MESSAGE_H */
