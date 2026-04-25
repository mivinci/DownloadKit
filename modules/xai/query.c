/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query.c - Placeholder translation unit for xai Query
 *
 * Part of Step 2 of the Session/Query split (see query.h and
 * docs/todo/human-like-ai.md §8). Introduced empty so that the CMake
 * GLOB_RECURSE rule picks up the new source file and the build stays
 * green before any behavioural change lands.
 *
 * The actual Query implementation — xAiQuery_ struct, xAiQueryCreate,
 * xAiQueryRun, xAiQueryCancel and the provider/tool-loop machinery
 * currently living inside session.c — migrates here in follow-up
 * commits of this same refactor.
 */

#include "query_private.h"

/*
 * Intentionally empty. A translation unit with no external symbols is
 * well-formed C; the linker simply ignores it. Keeping this file
 * around (rather than deferring its creation) lets every follow-up
 * commit be a pure move-of-code rather than a "create + move" combo,
 * which is nicer to review.
 */
