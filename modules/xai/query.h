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
 * This file is a placeholder for Step 2 of the Session/Query split
 * (see docs/todo/human-like-ai.md §8). The public API will land in a
 * follow-up commit; including this header today is a no-op by design,
 * so downstream code can start depending on its location.
 */

#ifndef XAI_QUERY_H
#define XAI_QUERY_H

#include <xai/session.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Intentionally empty for now.
 *
 * The Query API (xAiQuery handle, xAiQueryCreate / xAiQueryRun /
 * xAiQueryCancel, origin enum, ...) will be introduced in the next
 * commit of the Session/Query split. Keeping this header committable
 * but empty means:
 *
 *   - build wiring (GLOB_RECURSE in CMakeLists.txt) is proven to pick
 *     the new translation unit up without any other source touching
 *     it yet;
 *
 *   - reviewers can see the intended file layout before any behaviour
 *     change is proposed;
 *
 *   - reverting the whole split is a single-file revert if we decide
 *     against it.
 */

#ifdef __cplusplus
}
#endif

#endif /* XAI_QUERY_H */
