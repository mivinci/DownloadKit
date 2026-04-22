/*
 * Copyright 2025 The xKit Authors. All rights reserved.
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
 * xAiMessageFromText) construct shallow views; callers that need
 * longer-lived copies must make their own.
 */

#ifndef XAI_MESSAGE_H
#define XAI_MESSAGE_H

#include <stddef.h>
#include <xbase/base.h>
#include <xbase/error.h>

/**
 * @brief Speaker role of a message.
 *
 * Mirrors the OpenAI / Anthropic chat role vocabulary. Zero value
 * (xAiRole_System) is chosen so that a zero-initialised xAiMessage
 * defaults to a system-role empty message.
 */
XDEF_ENUM(xAiRole){
  xAiRole_System    = 0, /**< System / developer instructions            */
  xAiRole_User      = 1, /**< End-user input                              */
  xAiRole_Assistant = 2, /**< Model output                                */
  xAiRole_Tool      = 3, /**< Tool result fed back to the model           */
};

/**
 * @brief Content block kind carried by a message.
 */
XDEF_ENUM(xAiContentType){
  xAiContentType_Text       = 0, /**< Plain text block                    */
  xAiContentType_ToolUse    = 1, /**< Model-issued tool invocation        */
  xAiContentType_ToolResult = 2, /**< Tool execution result               */
};

/**
 * @brief A single content block inside a message.
 *
 * Tagged union: the @p type field selects which member of @p u is
 * active. All pointer fields are borrowed; the library never owns the
 * backing memory. Strings are NUL-terminated unless an explicit length
 * field is provided alongside.
 */
XDEF_STRUCT(xAiContent) {
  xAiContentType type;
  union {
    /** Active when type == xAiContentType_Text. */
    struct {
      const char *text; /**< UTF-8 text (must not be NULL)                */
      size_t      len;  /**< Byte length of @p text                       */
    } text;
    /** Active when type == xAiContentType_ToolUse. */
    struct {
      const char *id;        /**< Call id assigned by the provider        */
      const char *name;      /**< Tool name being invoked                 */
      const char *args_json; /**< Arguments as a JSON object (may be "{}")*/
    } tool_use;
    /** Active when type == xAiContentType_ToolResult. */
    struct {
      const char *id;         /**< Matching tool_use id                   */
      const char *output;     /**< Result payload (text or JSON)          */
      size_t      output_len; /**< Byte length of @p output               */
      int         is_error;   /**< Non-zero if the tool reported failure  */
    } tool_result;
  } u;
};

/**
 * @brief A message: one role plus an ordered list of content blocks.
 *
 * Zero-initialised xAiMessage is a valid empty system message. The
 * @p contents pointer is borrowed; the library never frees it.
 */
XDEF_STRUCT(xAiMessage) {
  xAiRole           role;     /**< Speaker role                           */
  const xAiContent *contents; /**< Array of @p n content blocks, or NULL  */
  size_t            n;        /**< Number of content blocks               */
};

/* ── Convenience builders (borrow-only, no allocation) ─────────────── */

/**
 * @brief Build a single text content block from a NUL-terminated string.
 *
 * The returned struct borrows @p text; the caller must keep @p text
 * alive for as long as the returned content is used.
 *
 * @param text  NUL-terminated UTF-8 string (must not be NULL).
 * @return      A text content block (type = xAiContentType_Text).
 */
XCAPI(xAiContent) xAiContentText(const char *text);

/**
 * @brief Build a user-role message containing a single text block.
 *
 * The returned message borrows @p text via an internal, caller-owned
 * xAiContent. Typical usage:
 *
 * @code
 *   xAiContent c = xAiContentText("hello");
 *   xAiMessage m = xAiMessageFromContent(xAiRole_User, &c, 1);
 *   xAiSessionInput(sess, m);
 * @endcode
 *
 * For the very common "send one user string" case, @ref
 * xAiMessageFromText wraps this pattern using a thread-local storage
 * slot so the caller does not have to manage the xAiContent lifetime.
 *
 * @param role     Speaker role.
 * @param contents Array of content blocks (may be NULL iff @p n == 0).
 * @param n        Number of content blocks.
 * @return         A message view.
 */
XCAPI(xAiMessage) xAiMessageFromContent(xAiRole           role,
                                        const xAiContent *contents, size_t n);

/**
 * @brief Build a user-role message carrying a single text block.
 *
 * Convenience for `xAiSessionInput(sess, xAiMessageFromText("..."))`.
 * The returned message references an internal per-thread buffer; the
 * view is valid until the next call to xAiMessageFromText() on the
 * same thread. Do not store the returned value across calls.
 *
 * @param text  NUL-terminated UTF-8 string (must not be NULL).
 * @return      A user-role message with one text block.
 */
XCAPI(xAiMessage) xAiMessageFromText(const char *text);

#endif /* XAI_MESSAGE_H */
