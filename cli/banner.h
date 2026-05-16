/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * banner.h - startup banner for the moo CLI.
 *
 * One-shot, cooked-mode print that sits between argv parsing and
 * the xline prompt. Renders a 72-col bordered box with:
 *
 *   - title row     "┌─ MOO <version> ───...───┐"
 *   - an ASCII-art logo on the left (3 rows, 17 cols, pure ASCII),
 *     picked at random from a small curated set so the startup
 *     feels a bit alive without drifting off-brand;
 *   - right column with model/data_dir knobs when a model is
 *     configured, or an empty right column + a yellow wrap-block
 *     hint below when models.json is missing/empty (degraded mode);
 *   - a one-line tips strip at the bottom.
 *
 * Width accounting assumes pure-ASCII body lines (byte == display
 * width); all logos are statically verified to be exactly 17 cells
 * wide per row. The title uses Unicode box-drawing, which is fine
 * on any modern terminal.
 *
 * `model_label` and `tools_label` are user-supplied text that land
 * in the right column; both are truncated to fit the 49-col field
 * so a long model id can't blow the frame.
 *
 * Pass no_models=1 to force the degraded layout even if the other
 * labels are populated; the caller (main.cpp) already has that
 * bit from the config loader. */
#ifndef MOO_APPS_CLI_BANNER_H
#define MOO_APPS_CLI_BANNER_H

#ifdef __cplusplus
extern "C" {
#endif

void banner_print(const char *version, const char *model_label, const char *tools_label,
                  const char *data_dir, int no_models);

#ifdef __cplusplus
}
#endif

#endif /* MOO_APPS_CLI_BANNER_H */
