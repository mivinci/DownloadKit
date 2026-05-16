/*
 * Copyright 2026 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * flag.h - Command-line flag parser (POSIX/GNU-compatible)
 *
 * A self-contained argv parser that replaces ad-hoc getopt(3) usage in
 * examples and applications. Produces structured values in
 * caller-owned storage and auto-generates a usage screen.
 *
 * ── Supported syntax ─────────────────────────────────────
 *
 *   -f value              short with argument
 *   -fvalue               short with argument (glued)
 *   -abc                  bundled no-arg shorts; last one may take arg
 *   --file value          long with argument
 *   --file=value          long with argument (=-form)
 *   --flag                long boolean / counter
 *   --                    end-of-options; rest are positional
 *   -                     treated as a positional argument (stdin idiom)
 *
 * ── Not supported (deliberately, in v1) ──────────────────
 *
 *   - subcommand trees (leave to a future xcli module)
 *   - environment / config-file fallback
 *   - shell-completion generation
 *   - long-name prefix matching (--fi for --file): requires exact match
 *   - i18n
 *   - dynamic registration after xFlagParse() starts
 *
 * ── Lifecycle ────────────────────────────────────────────
 *
 *   xFlagSet set = xFlagSetCreate("prog", "one-line summary");
 *   bool     ipv6   = false;
 *   const char *url = NULL;
 *   xFlagAddBool  (set, "ipv6",  '6', "enable IPv6", &ipv6,
 *                  xFlagAttr_None);
 *   xFlagAddString(set, "url",   'u', "URL",
 *                  "signal server", &url,
 *                  "ws://127.0.0.1:8080/ws",
 *                  xFlagAttr_None);
 *   char  *err = NULL;
 *   xErrno rc  = xFlagParse(set, argc, argv, &err);
 *   if (rc == xErrno_Again) { xFlagSetDestroy(set); return 0; }
 *   if (rc != xErrno_Ok) {
 *     fprintf(stderr, "%s\n", err ? err : "parse error");
 *     free(err);
 *     xFlagSetDestroy(set);
 *     return 1;
 *   }
 *   ... use ipv6 / url ...
 *   xFlagSetDestroy(set);
 *
 * ── Memory ───────────────────────────────────────────────
 *
 *   - xFlagSet copies name/help/meta/default strings it receives.
 *   - Parsed string values point into argv memory (zero-copy, like
 *     getopt's optarg). Callers must strdup to persist them past
 *     argv's lifetime.
 *   - xFlagAttr_Multi and xFlagAddPositionalTail allocate arrays
 *     owned by xFlagSet; they are freed by xFlagSetDestroy.
 */

#ifndef XBASE_FLAG_H
#define XBASE_FLAG_H

#include <stdint.h>
#include <x/base/base.h>
#include <x/base/error.h>

/* ───────────────────── Type ───────────────────── */

/**
 * @brief Opaque handle representing a set of registered flags.
 *
 * One xFlagSet usually corresponds to one program (or one subcommand
 * once a future xcli module wraps it).
 */
XDEF_HANDLE(xFlagSet);

/**
 * @brief Per-flag attribute bitmask.
 *
 * Combine with bitwise OR. Passed as the @c attrs argument to each
 * xFlagAdd* function.
 */
XDEF_ENUM(xFlagAttr){
  xFlagAttr_None     = 0,
  xFlagAttr_Required = 1 << 0, /**< Parse fails if the flag is absent  */
  xFlagAttr_Hidden   = 1 << 1, /**< Omit from --help output            */
  xFlagAttr_Multi    = 1 << 2, /**< Allow repetition; collect into an
                                    internal NUL-terminated array; only
                                    meaningful for string flags        */
};

/* ───────────────────── Lifecycle ───────────────────── */

/**
 * @brief Create a new flag set.
 *
 * @param prog     Program name shown in usage (must not be NULL).
 *                 Typically @c argv[0] or a fixed string.
 * @param summary  One-line summary shown under the program name
 *                 (may be NULL).
 * @return New handle, or NULL on allocation failure.
 */
XCAPI(xFlagSet) xFlagSetCreate(const char *prog, const char *summary);

/**
 * @brief Destroy a flag set and release owned memory. NULL-safe.
 *
 * Does not touch caller-owned storage pointers passed to xFlagAdd*.
 */
XCAPI(void) xFlagSetDestroy(xFlagSet set);

/**
 * @brief Append an epilog section printed after the options block
 *        (e.g. "Examples:" or "Notes:"). May be NULL to clear.
 */
XCAPI(void) xFlagSetEpilog(xFlagSet set, const char *text);

/**
 * @brief Register a version string; enables --version / -V handling.
 *
 * When set, xFlagParse recognises --version (and -V if that short
 * name is free), prints the string on stdout and returns
 * @ref xErrno_Again. Pass NULL to disable.
 */
XCAPI(void) xFlagSetVersion(xFlagSet set, const char *version);

/* ───────────────────── Add: scalars ─────────────────────
 *
 *   name     Long name without leading dashes, e.g. "file". May be
 *            NULL to register a short-only flag. Must be unique.
 *   shortc   Single-character short name, e.g. 'f'. Pass 0 for
 *            long-only flags. Must be unique.
 *   meta     Placeholder shown in usage, e.g. "FILE". NULL means the
 *            flag takes no argument in usage formatting. Ignored by
 *            xFlagAddBool / xFlagAddCounter.
 *   help     One-line description. NULL → empty.
 *   storage  Pointer to caller-owned variable filled on successful
 *            parse. Must outlive xFlagParse().
 *   def      Default value written to *storage before parsing; also
 *            shown in usage as "[default: ...]".
 *   attrs    Bitmask of xFlagAttr values.
 *
 * All Add* functions return xErrno_Ok, xErrno_InvalidArg (bad
 * arguments), xErrno_AlreadyExists (duplicate name/shortc), or
 * xErrno_NoMemory.
 */

/**
 * @brief Register a string flag. e.g. `--url ws://...` / `-u ws://...`.
 */
XCAPI(xErrno) xFlagAddString(xFlagSet set, const char *name, char shortc,
                             const char *meta, const char *help,
                             const char **storage, const char *def,
                             int attrs);

/**
 * @brief Register a boolean switch. Present → true; takes no argument.
 *        The @c meta parameter does not apply.
 */
XCAPI(xErrno) xFlagAddBool(xFlagSet set, const char *name, char shortc,
                           const char *help, bool *storage, int attrs);

/**
 * @brief Register a signed 32-bit integer flag.
 *
 * Accepts decimal, 0x hex, 0b binary, and 0-prefixed octal.
 */
XCAPI(xErrno) xFlagAddInt(xFlagSet set, const char *name, char shortc,
                          const char *meta, const char *help,
                          int *storage, int def, int attrs);

/**
 * @brief Register a signed 64-bit integer flag.
 */
XCAPI(xErrno) xFlagAddI64(xFlagSet set, const char *name, char shortc,
                          const char *meta, const char *help,
                          int64_t *storage, int64_t def, int attrs);

/**
 * @brief Register an unsigned 64-bit integer flag.
 */
XCAPI(xErrno) xFlagAddU64(xFlagSet set, const char *name, char shortc,
                          const char *meta, const char *help,
                          uint64_t *storage, uint64_t def, int attrs);

/**
 * @brief Register a double-precision floating-point flag.
 */
XCAPI(xErrno) xFlagAddDouble(xFlagSet set, const char *name, char shortc,
                             const char *meta, const char *help,
                             double *storage, double def, int attrs);

/**
 * @brief Register a choice flag whose value must match one of
 *        @p choices (NULL-terminated array of C strings).
 *
 * The array itself is not copied and must outlive @p set.
 * On mismatch, xFlagParse fails with xErrno_InvalidArg and fills
 * @c err_out with a list of valid choices.
 */
XCAPI(xErrno) xFlagAddChoice(xFlagSet set, const char *name, char shortc,
                             const char *meta, const char *help,
                             const char *const *choices,
                             const char **storage, const char *def,
                             int attrs);

/**
 * @brief Register a counter flag. Each occurrence increments
 *        @c *storage by 1 (e.g. `-vvv` or `-v -v -v` → 3).
 *
 * Takes no argument. @c *storage is initialised to 0 by xFlagParse
 * before processing argv.
 */
XCAPI(xErrno) xFlagAddCounter(xFlagSet set, const char *name, char shortc,
                              const char *help, int *storage, int attrs);

/* ───────────────────── Add: positional ───────────────────── */

/**
 * @brief Register a single positional argument.
 *
 * Positionals are matched in the order they are registered. Mark
 * required ones with @ref xFlagAttr_Required.
 *
 * @param name     Placeholder shown in usage, e.g. "INPUT".
 * @param help     One-line description (may be NULL).
 * @param storage  Receives the raw argv pointer (zero-copy).
 */
XCAPI(xErrno) xFlagAddPositional(xFlagSet set, const char *name,
                                 const char *help, const char **storage,
                                 int attrs);

/**
 * @brief Register a tail positional that captures all remaining
 *        argv after the previously registered positionals.
 *
 * Only one tail positional is allowed per set; it must be the last
 * positional registered. The resulting NUL-terminated array is
 * allocated and owned by the set (freed by xFlagSetDestroy).
 *
 * @param storage  Receives pointer to a NUL-terminated array of
 *                 @c const char * pointing into argv.
 * @param count    Optional out-parameter for element count; may be NULL.
 */
XCAPI(xErrno) xFlagAddPositionalTail(xFlagSet set, const char *name,
                                     const char *help,
                                     const char ***storage, size_t *count,
                                     int attrs);

/* ───────────────────── Parse ───────────────────── */

/**
 * @brief Parse argv and populate every registered storage pointer.
 *
 * @param set      Flag set; must have at least one xFlagAdd* call.
 * @param argc     Argument count (typically from main()).
 * @param argv     Argument vector; not modified.
 * @param err_out  Optional out-parameter; on failure receives a
 *                 newly-allocated one-line explanation that the
 *                 caller must free(). Pass NULL to discard.
 *
 * @retval xErrno_Ok          All flags resolved; storage populated.
 * @retval xErrno_Again       The user requested --help or --version;
 *                            usage/version has already been printed
 *                            on stdout. Caller should exit 0.
 * @retval xErrno_InvalidArg  Unknown flag, missing argument, type
 *                            conversion failure, missing required
 *                            flag, or bad choice. @c err_out filled.
 * @retval xErrno_NoMemory    Allocation failed while parsing.
 *
 * xFlagParse never calls exit(); the caller decides.
 */
XCAPI(xErrno) xFlagParse(xFlagSet set, int argc, char *const argv[],
                         char **err_out);

/* ───────────────────── Output ───────────────────── */

/**
 * @brief Print the one-line "USAGE: ..." summary line to @p fp.
 *
 * @param fp  @c FILE* (declared void* to keep <stdio.h> out of this
 *            header). Typically stdout or stderr.
 */
XCAPI(void) xFlagPrintUsage(xFlagSet set, void *fp);

/**
 * @brief Print the full help screen (usage + options + epilog) to
 *        @p fp. Same signature convention as xFlagPrintUsage.
 */
XCAPI(void) xFlagPrintHelp(xFlagSet set, void *fp);

#endif /* XBASE_FLAG_H */
