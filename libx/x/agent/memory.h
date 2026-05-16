/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory.h - Pluggable long-term memory store for the xai agent core
 *
 * An xAgentMemory abstracts "where and how do we keep history across
 * sessions". The agent wires a JSONL file writer into every session
 * via xAgentSessionConf::memory; that works for the simple "append
 * everything to disk" case but gives the session layer no way to
 * pull context back in on the NEXT run, and no way to plug in
 * smarter backends (summarisation, vector search, remote services)
 * without surgery in agent.c.
 *
 * The store model fixes both:
 *   - The persistence direction ("session → disk") is expressed as
 *     xAgentMemoryAppend(), routed through a vtable the caller can
 *     override.
 *   - The retrieval direction ("disk → session") is expressed as
 *     xAgentMemoryRetrieve(), consulted by the session layer on every
 *     xAgentSessionInput BEFORE the budget gate so hits become part
 *     of the token budget.
 *
 * Lifetime & ownership:
 *   - The caller creates an xAgentMemory and keeps it alive longer
 *     than the agent it is attached to.
 *   - The agent only borrows the handle — wiring happens at create
 *     time via xAgentConf (follow-up change; this header ships the
 *     store type independently so callers can build against it
 *     today).
 *   - Sessions never own a memory handle directly; they reach the
 *     store through the agent.
 *
 * Threading:
 *   - All xAgentMemory* APIs must be called on the agent's event
 *     loop thread. Implementations are free to spawn background
 *     I/O internally but must marshal completions back to the
 *     loop before invoking any caller callback.
 *
 * NOTE: this module ships the abstraction + one built-in backend
 *       (xAgentMemoryJsonlCreate) only. Agent / session wiring is a
 *       follow-up commit; existing callers see no behaviour change
 *       until they opt in by passing an xAgentMemory to the agent.
 */

#ifndef XAGENT_MEMORY_H
#define XAGENT_MEMORY_H

#include <stddef.h>
#include <x/agent/message.h> /* xAgentRole                           */
#include <x/agent/session.h> /* xAgentSessionMsg, xAgentSessionEntryKind */
#include <x/base/base.h>
#include <x/base/error.h>

/**
 * @brief Opaque handle to a memory store.
 *
 * A store is an instance of some backend (JSONL file, sqlite,
 * remote service, ...) bound to at most one agent at a time. Create
 * with a backend-specific factory (e.g. xAgentMemoryJsonlCreate),
 * attach to an agent through xAgentConf, and tear down with
 * xAgentMemoryDestroy AFTER the agent has been destroyed.
 */
XDEF_HANDLE(xAgentMemory);

/**
 * @brief A retrieval query issued by the session layer.
 *
 * Populated by the session immediately before the budget gate runs.
 * Everything inside is borrowed for the duration of the call — the
 * store must copy what it wants to keep.
 */
XDEF_STRUCT(xAgentMemoryQuery) {
  /**
   * @brief Session issuing the retrieve.
   *
   * Stores that key entries by session_id use this to scope the
   * search; MUST be non-NULL for append-style backends (the built-
   * in JSONL store rejects NULL with xErrno_InvalidArg). Borrowed;
   * never NUL-free'd.
   */
  const char *session_id;

  /**
   * @brief The user message that triggered this retrieve.
   *
   * May be NULL when the session is priming at create time rather
   * than responding to a turn. Implementations that do similarity
   * search use this as the query vector source. Pointer is valid
   * only for the duration of the call.
   */
  const xAgentMessage *recent_turn;

  /**
   * @brief Soft token budget the session is willing to spend on
   *        retrieved context.
   *
   * Implementations SHOULD clip their result set so the combined
   * text of all returned entries stays under this limit. Zero
   * means "no hint; return what you would return by default".
   * This is advisory — the session clips again after the fact if
   * the store over-shoots.
   */
  size_t budget_tokens;

  /**
   * @brief Upper bound on the number of entries to return.
   *
   * Zero means "no limit beyond @ref budget_tokens". Useful for
   * backends that have no token estimator of their own — they can
   * honour a simple count cap.
   */
  size_t max_entries;
};

/**
 * @brief Result of a retrieval.
 *
 * The store allocates @ref entries; the caller consumes them
 * read-only and then releases the whole result via
 * xAgentMemoryReleaseHits(). Individual string pointers inside
 * each xAgentSessionMsg point into store-owned storage and are
 * valid only until Release is called.
 */
XDEF_STRUCT(xAgentMemoryHits) {
  xAgentSessionMsg *entries;   /**< store-owned, read-only          */
  size_t            n_entries; /**< number of valid entries         */
  void             *cookie;    /**< opaque per-result state; the
                                    store uses this in Release to
                                    find the allocation to free    */
};

/**
 * @brief Reason an Append batch was delivered.
 *
 * New values may be added over time; stores MUST tolerate unknown
 * values.
 */
XDEF_ENUM(xAgentMemoryAppendReason){
  xAgentMemoryAppendReason_Truncated = 0,  /**< session trimmed old entries */
  xAgentMemoryAppendReason_Compacted = 1,  /**< session replaced entries
                                            with a summary              */
  xAgentMemoryAppendReason_Finalizing = 2, /**< session shutting down      */
  xAgentMemoryAppendReason_Explicit   = 3, /**< host called Append directly */
};

/**
 * @brief Backend vtable.
 *
 * Every function pointer except @ref destroy is optional —
 * implementations that do not implement a given operation MUST
 * leave the slot NULL, and the generic xAgentMemory* wrappers
 * will fall back to a sensible default (no-op for sinks like
 * on_session_open, empty hit set for retrieve, etc.).
 *
 * Callbacks are invoked synchronously on the agent's event loop
 * thread. Backends that do real I/O should keep their on-loop work
 * small and move blocking work onto a private task group.
 */
XDEF_STRUCT(xAgentMemoryVTable) {
  /**
   * @brief Persist a batch of history entries.
   *
   * @param store    Store handle.
   * @param query    Scope (session_id). Only the id field is
   *                 meaningful here; recent_turn and the budget
   *                 fields are ignored.
   * @param reason   Why this batch is being persisted.
   * @param msgs     Read-only array of entries to persist.
   * @param n_msgs   Number of entries in @p msgs.
   * @return         xErrno_Ok on success; any non-zero error is
   *                 swallowed by the generic layer but surfaced
   *                 through logs.
   */
  xErrno (*append)(xAgentMemory store, const xAgentMemoryQuery *query,
                   xAgentMemoryAppendReason reason, const xAgentSessionMsg *msgs, size_t n_msgs);

  /**
   * @brief Fetch relevant context for the upcoming turn.
   *
   * @param store  Store handle.
   * @param query  Scope + relevance hints.
   * @param out    Output; on success the store populates @p out
   *               with an allocation it owns until
   *               xAgentMemoryReleaseHits is called. MUST be
   *               zero-initialised on failure.
   * @return       xErrno_Ok on success (including the empty-hit
   *               case); xErrno_NotSupported if this backend does
   *               not implement retrieval.
   */
  xErrno (*retrieve)(xAgentMemory store, const xAgentMemoryQuery *query, xAgentMemoryHits *out);

  /**
   * @brief Release a result previously returned by @ref retrieve.
   *
   * Must be a no-op when @p hits->entries is NULL / n_entries is
   * zero. Backends are responsible for freeing their own per-hit
   * storage; the generic layer only zeroes the out-struct after
   * the call returns.
   */
  void (*release)(xAgentMemory store, xAgentMemoryHits *hits);

  /**
   * @brief Optional: called the first time a session is bound to
   *        this store.
   *
   * Backends can use this to warm caches, open files, or load an
   * index for the session. Called at most once per (session_id,
   * store) pair; safe to leave NULL.
   */
  xErrno (*on_session_open)(xAgentMemory store, const char *session_id);

  /**
   * @brief Optional: mirror of @ref on_session_open fired at
   *        session teardown AFTER the final Append.
   */
  xErrno (*on_session_close)(xAgentMemory store, const char *session_id);

  /**
   * @brief Destroy the backend instance.
   *
   * Called exactly once by xAgentMemoryDestroy. Must free all
   * per-instance state; must NOT touch the generic xAgentMemory_
   * wrapper (the wrapper frees itself).
   */
  void (*destroy)(xAgentMemory store);
};

/* ── Generic API — thin wrappers over the vtable ──────────────────── */

/**
 * @brief Persist a batch of entries.
 *
 * Thin wrapper around vt->append with parameter validation. Safe to
 * call with NULL @p store (no-op, returns xErrno_Ok) so callers can
 * treat "no memory configured" uniformly.
 */
XCAPI(xErrno) xAgentMemoryAppend(xAgentMemory store, const xAgentMemoryQuery *query,
                                 xAgentMemoryAppendReason reason, const xAgentSessionMsg *msgs,
                                 size_t n_msgs);

/**
 * @brief Retrieve relevant context.
 *
 * Thin wrapper around vt->retrieve. On success the caller MUST
 * eventually call xAgentMemoryReleaseHits to free the result.
 * With NULL @p store the call succeeds with an empty result.
 */
XCAPI(xErrno) xAgentMemoryRetrieve(xAgentMemory store, const xAgentMemoryQuery *query,
                                   xAgentMemoryHits *out);

/**
 * @brief Release a previously-retrieved hit set. Zeroes @p hits.
 */
XCAPI(void) xAgentMemoryReleaseHits(xAgentMemory store, xAgentMemoryHits *hits);

/**
 * @brief Notify the store that a session is about to start running
 *        against it. No-op when either the store or its vtable
 *        does not implement on_session_open.
 */
XCAPI(xErrno) xAgentMemoryOpenSession(xAgentMemory store, const char *session_id);

/**
 * @brief Notify the store that a session has just torn down.
 */
XCAPI(xErrno) xAgentMemoryCloseSession(xAgentMemory store, const char *session_id);

/**
 * @brief Destroy a memory store. NULL is a no-op.
 *
 * The caller must ensure no agent is still borrowing the store
 * (destroy the agent first, then the store). Pending retrieves
 * issued by the agent have already completed by the time the
 * agent is gone.
 */
XCAPI(void) xAgentMemoryDestroy(xAgentMemory store);

/* ── Built-in backend: JSONL file per session ──────────────────────── */

/**
 * @brief Configuration for the built-in JSONL backend.
 *
 * Each session gets its own append-only file at
 *   {root_dir}/sessions/{session_id}/history.jsonl
 * and retrieval reads the tail of that file, newest-first. It is
 * intentionally simple — no indexing, no summarisation, no vector
 * search — so callers can compose it with smarter layers on top.
 */
XDEF_STRUCT(xAgentMemoryJsonlConf) {
  /**
   * @brief Root directory under which per-session files are
   *        created.
   *
   * Borrowed from the caller for the store's lifetime. The
   * backend calls mkdir -p as needed. When NULL the factory
   * fails with xErrno_InvalidArg.
   */
  const char *root_dir;

  /**
   * @brief Upper bound on the number of entries Retrieve may
   *        return for a single call.
   *
   * Zero means "library default" (currently 64). The caller's
   * xAgentMemoryQuery::max_entries, when non-zero, further clips
   * this per call.
   */
  size_t default_max_entries;
};

/**
 * @brief Create the built-in JSONL memory backend.
 *
 * @param conf  Configuration (must not be NULL, root_dir must be
 *              set). Captured by value.
 * @return      A new xAgentMemory handle, or NULL on failure.
 */
XCAPI(xAgentMemory) xAgentMemoryJsonlCreate(const xAgentMemoryJsonlConf *conf);

#endif /* XAGENT_MEMORY_H */
