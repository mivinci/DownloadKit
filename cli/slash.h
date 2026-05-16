/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slash.h - Slash command dispatch and Tab-completion for the REPL.
 *
 * Commands typed at the prompt that start with '/' are intercepted by
 * the REPL and never sent to the model. See slash.cpp for the full
 * rationale and the per-handler commentary.
 */

#ifndef MOO_APPS_CLI_SLASH_H
#define MOO_APPS_CLI_SLASH_H

#include "ctx.h"

#include <x/line/line.h>

/* Register the slash-command completer with xline. Called once from
 * main() after xLineSetHistory. `ctx` is stashed as the xline
 * completer arg so per-command argument completers (e.g. /model
 * <id>) can reach the registry / session state. */
void slash_install_completer(ReplCtx *ctx);

/* Dispatch a raw input line starting with '/'. Returns true if the
 * line was a slash command (recognised or not — unknown commands
 * print a hint); caller should then skip the model-submit path. */
bool slash_dispatch(ReplCtx *ctx, const char *line);

#endif /* MOO_APPS_CLI_SLASH_H */
