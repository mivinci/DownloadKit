/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * message.c - Convenience constructors for xAgentContent / xAgentMessage
 *
 * These helpers produce borrow-only views; no allocations survive the
 * call except for xAgentMessageFromText, which parks its one-content
 * scratch in a thread-local buffer so the caller can pass the return
 * value straight to xAgentSessionInput() without juggling an extra
 * xAgentContent on their own stack.
 */

#include <x/agent/message.h>

#include <stddef.h>
#include <string.h>

/* ── Thread-local scratch for xAgentMessageFromText ──────────────────────── */

#if defined(_MSC_VER)
  #define XAGENT_TLS __declspec(thread)
#else
  #define XAGENT_TLS __thread
#endif

static XAGENT_TLS xAgentContent tls_text_slot_;

/* ── Public helpers ───────────────────────────────────────────────────── */

xAgentContent xAgentContentText(const char *text) {
  xAgentContent c;
  memset(&c, 0, sizeof(c));
  c.type       = xAgentContentType_Text;
  c.u.text.text = text;
  c.u.text.len  = text ? strlen(text) : 0;
  return c;
}

xAgentMessage xAgentMessageFromContent(xAgentRole role, const xAgentContent *contents,
                                 size_t n) {
  xAgentMessage m;
  memset(&m, 0, sizeof(m));
  m.role     = role;
  m.contents = contents;
  m.n        = n;
  return m;
}

xAgentMessage xAgentMessageFromText(const char *text) {
  tls_text_slot_ = xAgentContentText(text);
  return xAgentMessageFromContent(xAgentRole_User, &tls_text_slot_, 1);
}
