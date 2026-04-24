/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * message.c - Convenience constructors for xAiContent / xAiMessage
 *
 * These helpers produce borrow-only views; no allocations survive the
 * call except for xAiMessageFromText, which parks its one-content
 * scratch in a thread-local buffer so the caller can pass the return
 * value straight to xAiSessionInput() without juggling an extra
 * xAiContent on their own stack.
 */

#include <xai/message.h>

#include <stddef.h>
#include <string.h>

/* ── Thread-local scratch for xAiMessageFromText ──────────────────────── */

#if defined(_MSC_VER)
  #define XAI_TLS __declspec(thread)
#else
  #define XAI_TLS __thread
#endif

static XAI_TLS xAiContent tls_text_slot_;

/* ── Public helpers ───────────────────────────────────────────────────── */

xAiContent xAiContentText(const char *text) {
  xAiContent c;
  memset(&c, 0, sizeof(c));
  c.type       = xAiContentType_Text;
  c.u.text.text = text;
  c.u.text.len  = text ? strlen(text) : 0;
  return c;
}

xAiMessage xAiMessageFromContent(xAiRole role, const xAiContent *contents,
                                 size_t n) {
  xAiMessage m;
  memset(&m, 0, sizeof(m));
  m.role     = role;
  m.contents = contents;
  m.n        = n;
  return m;
}

xAiMessage xAiMessageFromText(const char *text) {
  tls_text_slot_ = xAiContentText(text);
  return xAiMessageFromContent(xAiRole_User, &tls_text_slot_, 1);
}
