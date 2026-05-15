/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash_cmd.h - Internal declarations for slash command handlers
 *               that live in their own translation units.
 *
 * Included only by slash.cpp (the command table) and the individual
 * slash_<cmd>.cpp files. Not a public header.
 */

#ifndef MOO_CLI_SLASH_CMD_H
#define MOO_CLI_SLASH_CMD_H

#include "ctx.h"

#include <xline/line.h>

/* /model */
void slash_cmd_model(ReplCtx *ctx, const char *args);
void slash_argc_model(xLineCompletionEnv cenv, ReplCtx *ctx,
                      const char *token);

/* /bypass */
void slash_cmd_bypass(ReplCtx *ctx, const char *args);
void slash_argc_bypass(xLineCompletionEnv cenv, ReplCtx *ctx,
                       const char *token);

/* /renderer */
void slash_cmd_renderer(ReplCtx *ctx, const char *args);
void slash_argc_renderer(xLineCompletionEnv cenv, ReplCtx *ctx,
                         const char *token);

/* /verbose */
void slash_cmd_verbose(ReplCtx *ctx, const char *args);
void slash_argc_verbose(xLineCompletionEnv cenv, ReplCtx *ctx,
                        const char *token);

#endif /* MOO_CLI_SLASH_CMD_H */
