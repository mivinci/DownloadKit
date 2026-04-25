/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * query_private.h - Internal layout of xAiQuery_
 *
 * Exposed only to query.c, session.c and their tests. Other xai
 * translation units must go through query.h.
 *
 * Placeholder for Step 2 of the Session/Query split. See query.h and
 * docs/todo/human-like-ai.md §8 for the full design rationale.
 */

#ifndef XAI_QUERY_PRIVATE_H
#define XAI_QUERY_PRIVATE_H

#include "query.h"

/*
 * struct xAiQuery_ will move here from session_private.h's
 * xAiSessionQueryState_ in the next commit of this split.
 *
 * For now this header exists purely to reserve the path; including it
 * is a no-op.
 */

#endif /* XAI_QUERY_PRIVATE_H */
