/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * output.h - Output helpers that funnel all AI-driven prints through
 *            xline's "above" channel so the prompt row stays intact.
 *
 * See the file-header comment in output.cpp for the rationale.
 */

#ifndef XKIT_APPS_CLI_OUTPUT_H
#define XKIT_APPS_CLI_OUTPUT_H

#include "ctx.h"

#include <cstddef>
#include <xline/line.h>

/* printf-style wrapper around xLinePrintAbove. Appends a trailing
 * newline via xLinePrintAbove's own policy, so callers should NOT
 * include one unless they want a blank line. */
void above_printf(xLineHandle h, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* Stream a non-NUL-terminated fragment through xLinePrintAboveChunk
 * by copying it into a temporary NUL-terminated buffer. Preserves
 * the "token-by-token" visual cadence (no forced newlines). */
void above_chunk(xLineHandle h, const char *s, size_t len);

/* Close an open "[thinking] …" block: reset SGR + blank row. Safe to
 * call when no thinking block is open (no-op). */
void end_thinking(ReplCtx *ctx);

#endif /* XKIT_APPS_CLI_OUTPUT_H */
