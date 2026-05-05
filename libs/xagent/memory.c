/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory.c - Generic dispatch layer for xAgentMemory
 *
 * Every public xAgentMemory* entry point in memory.h lives here as
 * a thin wrapper that validates its arguments and then routes to
 * the backend vtable attached to the handle. Backends themselves
 * (memory_jsonl.c, tests' mocks, ...) implement the vtable slots
 * and do all the real work.
 *
 * The dispatch layer deliberately tolerates a NULL store handle for
 * Append / Retrieve / Open / Close so that call-sites in session.c
 * can pass the agent's (possibly unset) memory handle through
 * without branching. "No memory configured" then collapses to a
 * no-op Append and an empty Retrieve result.
 */

#include "memory_private.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Append ──────────────────────────────────────────────────────── */

xErrno xAgentMemoryAppend(xAgentMemory store, const xAgentMemoryQuery *query,
                          xAgentMemoryAppendReason reason,
                          const xAgentSessionMsg *msgs, size_t n_msgs) {
  /* Tolerate NULL store — "no memory configured" → silently drop. */
  if (!store) return xErrno_Ok;
  if (!query) return xErrno_InvalidArg;
  /* Empty batch is a legal no-op: allows callers to fire on every
   * session tick without branching on msgs==NULL. */
  if (!msgs || n_msgs == 0) return xErrno_Ok;

  struct xAgentMemory_ *s = (struct xAgentMemory_ *)store;
  if (!s->vt || !s->vt->append) return xErrno_NotSupported;

  return s->vt->append(store, query, reason, msgs, n_msgs);
}

/* ── Retrieve ────────────────────────────────────────────────────── */

xErrno xAgentMemoryRetrieve(xAgentMemory store, const xAgentMemoryQuery *query,
                            xAgentMemoryHits *out) {
  if (!out) return xErrno_InvalidArg;
  /* Always zero the output so callers can unconditionally call
   * ReleaseHits on it even when we never populated anything. */
  memset(out, 0, sizeof(*out));

  if (!store) return xErrno_Ok; /* empty result */
  if (!query) return xErrno_InvalidArg;

  struct xAgentMemory_ *s = (struct xAgentMemory_ *)store;
  if (!s->vt || !s->vt->retrieve) return xErrno_Ok; /* empty result */

  return s->vt->retrieve(store, query, out);
}

/* ── ReleaseHits ─────────────────────────────────────────────────── */

void xAgentMemoryReleaseHits(xAgentMemory store, xAgentMemoryHits *hits) {
  if (!hits) return;
  if (!hits->entries && hits->n_entries == 0 && !hits->cookie) return;

  if (store) {
    struct xAgentMemory_ *s = (struct xAgentMemory_ *)store;
    if (s->vt && s->vt->release) s->vt->release(store, hits);
  }
  /* Zero the out-struct regardless so a double-release is harmless. */
  memset(hits, 0, sizeof(*hits));
}

/* ── OpenSession / CloseSession ──────────────────────────────────── */

xErrno xAgentMemoryOpenSession(xAgentMemory store, const char *session_id) {
  if (!store) return xErrno_Ok;
  struct xAgentMemory_ *s = (struct xAgentMemory_ *)store;
  if (!s->vt || !s->vt->on_session_open) return xErrno_Ok;
  return s->vt->on_session_open(store, session_id);
}

xErrno xAgentMemoryCloseSession(xAgentMemory store, const char *session_id) {
  if (!store) return xErrno_Ok;
  struct xAgentMemory_ *s = (struct xAgentMemory_ *)store;
  if (!s->vt || !s->vt->on_session_close) return xErrno_Ok;
  return s->vt->on_session_close(store, session_id);
}

/* ── Destroy ─────────────────────────────────────────────────────── */

void xAgentMemoryDestroy(xAgentMemory store) {
  if (!store) return;
  struct xAgentMemory_ *s = (struct xAgentMemory_ *)store;
  /* Backends are required to implement destroy — they allocated
   * the struct, they own the teardown. We only hit this path when
   * a backend author forgets to set the slot; be defensive and
   * leak rather than crash, so tests surface the bug without
   * destabilising callers. */
  if (s->vt && s->vt->destroy) s->vt->destroy(store);
}
