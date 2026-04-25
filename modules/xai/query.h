/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query.h - One run of the xai tool loop
 *
 * A Query represents a single end-to-end execution of the provider
 * tool loop:
 *
 *     user input  ──▶  provider round 1  ──▶  (tools?) ──▶  round 2
 *                                                   ...
 *                                         ──▶  on_done  ──▶  done
 *
 * Sessions own history and long-lived configuration; queries own the
 * transient state that lives exactly as long as "the model is running
 * right now". Separating the two lets a session host multiple queries
 * over its lifetime (sequentially for now; the surface is also ready
 * for a future where an agent can inject a SystemSynthesized query
 * alongside a user-initiated one).
 *
 * Lifetime & ownership (Phase α):
 *   - The Query is owned by its Session and has the same lifetime as
 *     the Session. There is no xAiQueryCreate / xAiQueryDestroy in
 *     this phase — Queries are produced internally by xAiSessionInput
 *     and surface only as handles read back from the Session.
 *   - A handle remains valid until xAiSessionDestroy() is called on
 *     the owning Session. Do NOT retain it past that point.
 *
 * Driving vs. observing:
 *   - Drive a run: keep using xAiSessionInput / xAiSessionCancel on
 *     the Session.
 *   - Observe or target the current run specifically: obtain the
 *     Query handle and use the functions declared here.
 *
 * Threading:
 *   - Every xAiQuery* call must happen on the owning agent's event
 *     loop, same as xAiSession* calls.
 */

#ifndef XAI_QUERY_H
#define XAI_QUERY_H

#include <stddef.h>
#include <xai/provider.h> /* xAiUsage                                  */
#include <xai/session.h>  /* xAiSession, xAiInputOrigin, xAiDoneReason */
#include <xbase/base.h>
#include <xbase/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to one run of the agent tool loop.
 *
 * A Query exists for the entire lifetime of its owning Session
 * (Phase α of the Session/Query split); between runs it is simply
 * in the idle state (xAiQueryIsRunning returns 0). Each accepted
 * xAiSessionInput() call arms the same Query for a fresh run and
 * ends by firing on_done exactly once.
 *
 * Handles are non-owning: destroying the Session invalidates every
 * outstanding Query handle derived from it. Do not free them.
 */
XDEF_HANDLE(xAiQuery);

/**
 * @brief Get the Query currently associated with a Session.
 *
 * Returns the single embedded Query that this Session uses to run
 * the tool loop. The returned handle is valid until
 * xAiSessionDestroy(@p sess) and aliases stable storage — calling
 * this twice on the same Session returns the same handle.
 *
 * The Query may be idle (no run in flight), in which case
 * xAiQueryIsRunning() returns 0 and xAiQueryUsage() yields all-zero
 * / all-unknown totals. Callers that only care about runs in
 * progress should gate on xAiQueryIsRunning().
 *
 * @param sess  Session handle.
 * @return      The Session's Query handle, or NULL if @p sess is NULL.
 */
XCAPI(xAiQuery) xAiSessionQuery(xAiSession sess);

/**
 * @brief Cancel this Query's in-flight run, if any.
 *
 * Requests the provider to stop streaming and any in-flight tool
 * handlers to bail out. The Session's on_done callback is still
 * delivered (with reason == xAiDoneReason_Aborted) once unwinding
 * completes. Calling this on a Query whose run is already finished
 * or never started is a silent no-op.
 *
 * Equivalent to xAiSessionCancel() on the Query's owning Session;
 * both entry points exist so callers can target whichever level
 * their code is already tracking.
 *
 * @param q  Query handle (NULL is a no-op).
 */
XCAPI(void) xAiQueryCancel(xAiQuery q);

/**
 * @brief Whether this Query currently has a run in flight.
 *
 * Returns 1 from the moment xAiSessionInput() accepts an input
 * until the matching on_done has been delivered; 0 otherwise (and
 * for a NULL handle).
 *
 * @param q  Query handle.
 * @return   1 if a run is in flight, 0 otherwise.
 */
XCAPI(int) xAiQueryIsRunning(xAiQuery q);

/**
 * @brief Cumulative token usage across every provider round of the
 *        current (or most recent) run.
 *
 * Copies the running totals into @p out. Each field uses -1 as the
 * "unknown" sentinel — providers that do not report a particular
 * counter leave that field as -1 even on successful runs.
 *
 * Before the first round of the first run ever reports usage, every
 * field is -1. Between runs the totals are reset, so reading after
 * on_done fires but before the next xAiSessionInput() returns the
 * final totals of the run that just ended; once the next run
 * starts, the accumulator restarts from zero / unknown.
 *
 * @param q    Query handle (NULL populates @p out with all -1).
 * @param out  Destination struct (must not be NULL).
 */
XCAPI(void) xAiQueryUsage(xAiQuery q, xAiUsage *out);

/**
 * @brief Read back the owning Session of this Query.
 *
 * Useful for callbacks that only carry the Query handle but want
 * to reach back into Session-level configuration or history.
 *
 * @param q  Query handle.
 * @return   The owning Session, or NULL for a NULL input.
 */
XCAPI(xAiSession) xAiQuerySession(xAiQuery q);

/**
 * @brief Provider round counter for the current (or most recent) run.
 *
 * Incremented just before each submit, so during the first round's
 * callbacks this reads 1, during the second round 2, and so on.
 * Between runs the counter is reset to 0. Reading on a NULL handle
 * or an idle Query returns 0.
 *
 * Primarily intended for diagnostics (progress bars, logs).
 *
 * @param q  Query handle.
 * @return   Number of provider submits issued so far in this run.
 */
XCAPI(int) xAiQueryTurn(xAiQuery q);

#ifdef __cplusplus
}
#endif

#endif /* XAI_QUERY_H */
