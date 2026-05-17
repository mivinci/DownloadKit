/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * md.c - Streaming markdown -> ANSI (P0 implementation).
 *
 * Design notes
 * ------------
 * The single tricky property of a streaming markdown renderer is
 * that delimiters can straddle chunk boundaries. The producer might
 * emit "**bo" then "ld**" as two separate deltas, and we still
 * need the consumer to see a single bold word.
 *
 * We address that with a minimal pending buffer that holds at most
 * three bytes - only the ones that could be a delimiter prefix
 * ('*', '**', '`', '``', '#', '##', '###', '\\'). Every other byte
 * flows straight through, flushing any accumulated pending as
 * literal on the way out (the "these two stars are followed by
 * punctuation, so they were just prose" path).
 *
 * Fenced code blocks are special: once we see ``` at bol, we enter
 * fence mode and EVERYTHING passes through untouched (including
 * further '*' and '_') until we see ``` at bol again. This is the
 * single most important invariant - getting it wrong means shell
 * snippets like `for i in *; do` end up partially italicised.
 *
 * Heading detection is cheap: at bol, a run of 1-3 '#' followed by
 * ' ' turns on heading, and the whole line (until '\n') is emitted
 * inside a bold SGR pair. We intentionally do NOT eat the '#'
 * characters themselves - they're useful visual markers even when
 * bolded - nor promote them to coloured sections. Keeping the
 * original bytes means copy-paste from the terminal still yields
 * valid markdown.
 *
 * SGR choices:
 *   bold   : \e[1m / \e[22m  (22 turns off both bold AND faint -
 *                             doesn't matter here, we never enter
 *                             faint inside the markdown stream.)
 *   italic : \e[3m / \e[23m
 *   code   : \e[4m / \e[24m  (underline; theme-agnostic and - unlike
 *                             reverse-video - never triggers the
 *                             terminal's "paint background to EOL"
 *                             behaviour when the close-SGR races
 *                             with a trailing newline across the
 *                             xline above-region streaming path.)
 *
 * Why underline for inline code? We tried reverse-video and 256-
 * colour backgrounds earlier; both look fine when the whole line
 * is flushed at once, but when the sink is called chunk-by-chunk
 * (one SGR open, one text run, one SGR close, one '\n') the
 * terminal can end up painting the reverse-video background all
 * the way to the right margin before the close-SGR lands, and on
 * macOS Terminal in particular that occasionally wedges the input
 * line. Underline has none of that: it decorates glyph cells only,
 * so a late \e[24m is a visual no-op, not a full-row repaint.
 */

#include <x/tui/md.h>

#include <string.h>

/* ── Sink helpers ──────────────────────────────────────────────── */

static void raw_n(xMd *md, const char *data, size_t len) {
  if (!md->sink || !data || len == 0) return;
  md->sink(data, len, md->arg);
  md->last_char = data[len - 1];
}

/* ── Pending-buffer helpers ────────────────────────────────────── */

static void pending_push(xMd *md, char c) {
  /* pending[] is 4 bytes; we only ever append up to 3 meaningful
   * prefix bytes (``` / *** / ### / \\X) so this guard is
   * defensive. If it ever triggers the state machine has a bug. */
  if (md->pending_n < sizeof(md->pending)) {
    md->pending[md->pending_n++] = c;
  }
}

static void flush_pending_literal(xMd *md) {
  if (md->pending_n == 0) return;
  /* Emit exactly what we buffered. Note: we don't re-run these
   * bytes through the dispatch path because that would re-enter
   * delimiter-detection and potentially re-buffer them forever.
   * Pending bytes are, by definition, already past that decision
   * point. */
  raw_n(md, md->pending, md->pending_n);
  md->pending_n = 0;
}

static void emit_literal(xMd *md, char c) {
  raw_n(md, &c, 1);
  md->bol = (c == '\n');
}

static int is_space_char(char x) {
  return x == ' ' || x == '\t' || x == '\n' || x == '\r';
}

static int is_word_char(char x) {
  return (x >= '0' && x <= '9') || (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z');
}

/* ── Style-toggle helpers ──────────────────────────────────────── */

static void toggle_bold(xMd *md) {
  if (md->bold) {
    raw_n(md, "\x1b[22m", 5);
    md->bold = 0;
  } else {
    raw_n(md, "\x1b[1m", 4);
    md->bold = 1;
  }
}

static void toggle_italic(xMd *md) {
  if (md->italic) {
    raw_n(md, "\x1b[23m", 5);
    md->italic = 0;
  } else {
    raw_n(md, "\x1b[3m", 4);
    md->italic = 1;
  }
}

static void toggle_code(xMd *md) {
  if (md->code) {
    raw_n(md, "\x1b[24m", 5);
    md->code = 0;
  } else {
    raw_n(md, "\x1b[4m", 4);
    md->code = 1;
  }
}

/* ── Public API ────────────────────────────────────────────────── */

void xMdInit(xMd *md, xMdSinkFunc sink, void *arg) {
  memset(md, 0, sizeof(*md));
  md->sink      = sink;
  md->arg       = arg;
  md->bol       = 1;
  md->last_char = '\n';
}

void xMdFeed(xMd *md, const char *data, size_t len) {
  if (!data || len == 0) return;

  /* ── Stitch UTF-8 pending tail from previous chunk ────────────
   *
   * If the last chunk ended mid-UTF-8 (e.g. 0xE4 arrived but its
   * two continuation bytes 0xBD 0xA0 are in this chunk), we
   * prepend the stashed bytes so the for-loop sees a complete
   * character.  We use a small on-stack buffer: up to 3 pending
   * bytes + the current chunk. */
  char        stitch[3 + len]; /* VLA, len is caller-controlled */
  const char *cur;
  size_t      cur_len;

  if (md->utf8_pending_n > 0) {
    memcpy(stitch, md->utf8_pending, md->utf8_pending_n);
    memcpy(stitch + md->utf8_pending_n, data, len);
    cur                = stitch;
    cur_len            = md->utf8_pending_n + len;
    md->utf8_pending_n = 0;
  } else {
    cur     = data;
    cur_len = len;
  }

  /* ── UTF-8 boundary check (before processing) ──────────────────
   *
   * If the chunk ends in the middle of a UTF-8 multi-byte
   * sequence, we must NOT feed the incomplete character to the
   * state machine — the leading byte would fall into the `default`
   * branch and be emitted as a lone byte (showing ? or � on the
   * terminal).  Instead, we trim the incomplete tail and stash it
   * in utf8_pending for the next call.
   *
   * We walk backwards from the end looking for the leading byte
   * of the last UTF-8 sequence.  If the sequence is incomplete
   * (fewer bytes available than the leading byte declares), the
   * partial bytes are moved to utf8_pending.
   *
   * UTF-8 encoding:
   *   0xxxxxxx                               - 1 byte  (ASCII)
   *   110xxxxx 10xxxxxx                      - 2 bytes
   *   1110xxxx 10xxxxxx 10xxxxxx             - 3 bytes
   *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx    - 4 bytes
   */
  if (cur_len > 0) {
    const unsigned char *base = (const unsigned char *)cur;
    const unsigned char *tail = base + cur_len;
    const unsigned char *p    = tail - 1;

    /* Find the last non-continuation byte. */
    while (p > base && (*p & 0xC0) == 0x80)
      --p;

    unsigned char lead    = *p;
    size_t        seq_len = 1; /* default: ASCII or orphan continuation */

    if (lead >= 0xF0)
      seq_len = 4;
    else if (lead >= 0xE0)
      seq_len = 3;
    else if (lead >= 0xC0)
      seq_len = 2;

    size_t avail = (size_t)(tail - p);
    if (seq_len > 1 && avail < seq_len) {
      /* Incomplete sequence — stash the partial bytes and trim
       * the input so the for-loop never sees them. */
      size_t stash = avail;
      memcpy(md->utf8_pending, p, stash);
      md->utf8_pending_n = stash;
      cur_len -= stash;
    }
  }

  for (size_t i = 0; i < cur_len; ++i) {
    char c = cur[i];

    /* ── Fence mode: raw pass-through until closing ``` at bol ──
     *
     * We still need to track bol inside the fence because the
     * close-fence detection depends on it. But we never look at
     * '*', '_', '`' for inline purposes here. The only thing we
     * watch for is ``` at the start of a line, which closes the
     * fence. Detection shares the pending buffer. */
    if (md->fence) {
      if (md->bol && c == '`') {
        pending_push(md, c);
        if (md->pending_n == 3) {
          /* Close fence. Emit the ``` literally (users expect to
           * see the fence bytes) and leave fence mode. bol stays
           * 0 - we've emitted three bytes on this line. */
          raw_n(md, md->pending, md->pending_n);
          md->pending_n = 0;
          md->fence     = 0;
          md->bol       = 0;
        }
        continue;
      }
      /* A non-backtick (or a backtick not at bol) aborts a
       * partial fence candidate - flush whatever we had and emit
       * this byte raw. */
      if (md->pending_n > 0) {
        raw_n(md, md->pending, md->pending_n);
        md->pending_n = 0;
      }
      raw_n(md, &c, 1);
      md->bol = (c == '\n');
      continue;
    }

    /* ── Resolve pending delimiter candidates ──────────────────
     *
     * Pending only ever holds delimiter prefixes. On each new
     * byte we decide whether pending + current forms a recognised
     * structure or not. Most of the complexity lives here; the
     * rest of the function is just "start buffering". */
    if (md->pending_n > 0) {
      char p0 = md->pending[0];

      /* "\\" + X: escape. Emit X literally (even if it's a
       * delimiter byte). Only honour the common markdown escapes
       * - otherwise a stray backslash in prose would swallow the
       * next char. */
      if (p0 == '\\') {
        md->pending_n = 0;
        if (c == '*' || c == '_' || c == '`' || c == '#' || c == '\\') {
          raw_n(md, &c, 1);
          md->bol = 0;
        } else {
          /* Not an escape we care about - emit both bytes raw. */
          char buf[2];
          buf[0] = '\\';
          buf[1] = c;
          raw_n(md, buf, 2);
          md->bol = (c == '\n');
        }
        continue;
      }

      /* "*" pending. Options:
       *   "*" + "*"  -> got "**": might still grow to "***",
       *                 buffer one more byte.
       *   "*" + X    -> italic toggle + emit X normally.
       *                 BUT: "* " at bol is a bullet list marker;
       *                 we don't support lists at P0 so fall
       *                 through to italic, which would look weird
       *                 (an unclosed italic until newline). To
       *                 avoid that footgun, if the byte after a
       *                 bol '*' is ' ', treat the '*' as literal.
       */
      if (p0 == '*' && md->pending_n == 1) {
        if (c == '*') {
          pending_push(md, c);
          continue;
        }
        /* Bullet-list defusing: literal '*' if followed by space
         * and we were at bol when we buffered. We detect bol
         * retroactively: pending only accepts '*' at non-fence
         * positions, and bol stays 1 only if no byte has been
         * emitted since - which happens exactly when the buffered
         * '*' was itself at bol. */
        if (md->bol && c == ' ') {
          flush_pending_literal(md);
          emit_literal(md, c);
          continue;
        }
        /* Italic toggle. */
        md->pending_n = 0;
        toggle_italic(md);
        /* fall through: emit current byte via normal path */
      } else if (p0 == '*' && md->pending_n == 2) {
        /* "**" buffered. */
        if (c == '*') {
          /* "***" - triple emphasis. Bold+italic toggle for
           * symmetrical close/open. */
          md->pending_n = 0;
          toggle_bold(md);
          toggle_italic(md);
          continue;
        }
        /* Bold toggle. */
        md->pending_n = 0;
        toggle_bold(md);
        /* fall through */
      } else if (p0 == '_') {
        /* "_" pending. Right-side discriminator:
         *   - If italic is already open, any byte closes it.
         *   - If italic is not open, the byte after '_' must be
         *     non-whitespace for this '_' to open a span; a
         *     whitespace after means the '_' was a stray
         *     underscore (e.g. "under _ score") and stays
         *     literal. */
        md->pending_n = 0;
        if (!md->italic && is_space_char(c)) {
          /* Literal underscore; emit the '_' first so the space
           * keeps its natural position. */
          raw_n(md, "_", 1);
          md->bol = 0;
          /* fall through to emit c normally */
        } else {
          toggle_italic(md);
          /* fall through */
        }
      } else if (p0 == '`') {
        /* "`" pending. Options:
         *   "`" + "`"   -> could be "``" empty inline or start of
         *                  ``` fence; keep buffering one more.
         *   "`" + X     -> inline code toggle.
         */
        if (md->pending_n == 1) {
          if (c == '`') {
            pending_push(md, c);
            continue;
          }
          md->pending_n = 0;
          toggle_code(md);
          /* fall through */
        } else if (md->pending_n == 2) {
          /* "``" buffered. */
          if (c == '`') {
            /* ``` - fence open if at bol, otherwise literal run
             * of three backticks (weird but legal). bol here
             * refers to the state before any of the three
             * backticks were emitted, because we never emitted
             * them. */
            md->pending_n = 0;
            if (md->bol) {
              /* Enter fence mode. Emit the ``` literally so the
               * user sees the fence. bol becomes 0. */
              raw_n(md, "```", 3);
              md->fence = 1;
              md->bol   = 0;
              continue;
            }
            /* Not bol - three literal backticks. */
            raw_n(md, "```", 3);
            md->bol = 0;
            continue;
          }
          /* "``" + non-backtick: "empty" inline code `` - emit
           * literally. Rare, not worth toggling. */
          md->pending_n = 0;
          raw_n(md, "``", 2);
          md->bol = 0;
          /* fall through to emit c */
        }
      } else if (p0 == '#') {
        /* "#" pending - heading candidate. Count consecutive '#'
         * (max 3 for h1/h2/h3) then require a space. We buffer
         * '#' runs only at bol. */
        if (c == '#' && md->pending_n < 3) {
          pending_push(md, c);
          continue;
        }
        if (c == ' ') {
          /* Confirmed heading. Emit the '#' run and the space
           * literally (keeps copy-paste markdown-valid) and open
           * a bold span for the rest of the line. */
          raw_n(md, md->pending, md->pending_n);
          raw_n(md, " ", 1);
          md->pending_n = 0;
          if (!md->bold) {
            raw_n(md, "\x1b[1m", 4);
            md->bold    = 1;
            md->heading = 1;
          } else {
            /* already bold for some reason - just mark heading
             * so we still close on newline */
            md->heading = 1;
          }
          md->bol = 0;
          continue;
        }
        /* Not a heading after all - emit buffered '#'s literally. */
        raw_n(md, md->pending, md->pending_n);
        md->pending_n = 0;
        md->bol       = 0;
        /* fall through to emit c */
      }
    }

    /* ── No pending state (or we just resolved it and fell through).
     *
     * Classify the current byte. */
    switch (c) {
    case '\\':
      pending_push(md, c);
      continue;
    case '*':
      pending_push(md, c);
      continue;
    case '_':
      /* Emphasis open/close rules (CommonMark-approx, P0):
       *   - If italic is already open, '_' closes it regardless
       *     of the left neighbour - that lets "foo_bar_" inside
       *     an emphasis span still close cleanly at the final
       *     '_'. (Closing too eagerly is safer than leaving
       *     emphasis hanging to Flush.)
       *   - If italic is NOT open, '_' only opens a span when
       *     the left neighbour is non-word. Otherwise this is an
       *     underscore inside snake_case and must stay literal.
       * The right-side check happens at resolution time. */
      if (!md->italic && is_word_char(md->last_char)) {
        emit_literal(md, c);
        continue;
      }
      pending_push(md, c);
      continue;
    case '`':
      pending_push(md, c);
      continue;
    case '#':
      if (md->bol) {
        pending_push(md, c);
        continue;
      }
      /* '#' in the middle of a line is prose (e.g. "PR #123"). */
      emit_literal(md, c);
      continue;
    case '\n':
      /* Close heading span on newline. Inline emphasis that
       * wasn't closed by end-of-line stays open - matches common
       * terminal behaviour and the producer almost always closes
       * its own pairs. If it doesn't, Flush will reset at
       * end-of-stream. */
      if (md->heading) {
        raw_n(md, "\x1b[22m", 5);
        md->heading = 0;
        md->bold    = 0;
      }
      emit_literal(md, c);
      continue;
    default:
      /* ── Bulk literal emission ──────────────────────────────────
       *
       * Non-delimiter bytes (ASCII printable, UTF-8 continuations,
       * high-byte prose) all flow through here. We scan forward to
       * find the longest contiguous run of such bytes and emit
       * them in a single raw_n() call instead of looping byte by
       * byte. This matters because each raw_n() triggers the sink,
       * which in the CLI drives above_chunk → xLinePrintAboveChunk
       * → term_flush — a write() to the tty. When a 3-byte UTF-8
       * character is emitted as three separate write()s, the
       * terminal receives an incomplete leading byte and renders a
       * question mark or replacement character. Batching the whole
       * run (including all bytes of the same UTF-8 character) into
       * one sink call ensures the terminal sees the complete
       * sequence atomically.
       *
       * A "literal run" is the longest span starting at i where
       * every byte is NOT one of the special delimiter/structure
       * bytes checked in the switch above ( \ * _ ` # \n ). UTF-8
       * continuation bytes (0x80–0xBF) are always literal; leading
       * bytes (0xC0–0xFD) are literal too (they're not markdown
       * delimiters). ASCII control chars other than \n that land
       * here are also emitted literally (they're rare in real
       * streams and treating them as literal matches the previous
       * behaviour). */
      {
        size_t start = i;
        size_t end   = i + 1; /* at least byte cur[i] is literal */
        while (end < cur_len) {
          char nc = cur[end];
          /* Stop at any byte that needs special handling. */
          if (nc == '\\' || nc == '*' || nc == '_' || nc == '`' || nc == '#' || nc == '\n') {
            break;
          }
          end++;
        }
        raw_n(md, cur + start, end - start);
        md->bol = 0;
        i       = end - 1; /* for-loop will ++i, so advance to end-1 */
      }
      continue;
    }
  }
}

void xMdFlush(xMd *md) {
  /* Drain any UTF-8 bytes that were held across chunk boundaries
   * but never completed (the stream ended mid-character). We emit
   * them as-is — they're incomplete UTF-8 but the alternative
   * (silently discarding) is worse. */
  if (md->utf8_pending_n > 0) {
    raw_n(md, md->utf8_pending, md->utf8_pending_n);
    md->utf8_pending_n = 0;
  }

  /* Pending resolution at end-of-stream. The buffer can hold the
   * "ambiguous" state of a multi-byte delimiter (e.g. "**"): at
   * the time we parked it we didn't know whether the next byte
   * would extend it to "***" or close a bold span. With no more
   * bytes coming, prefer emphasis over literal for the *complete*
   * forms - the producer almost certainly meant to close a span.
   *
   * Incomplete forms at EOF are resolved by looking at the open
   * style flags: a lone '*' or '_' when italic is open is a close
   * delimiter (the producer meant to end emphasis); when italic
   * is closed it stays literal ("2 * 3" case). Same rule for a
   * lone '`' against code. A lone '#' or '\\' has no corresponding
   * open state, so it's always literal. */
  if (md->pending_n > 0) {
    char p0 = md->pending[0];
    if (p0 == '*' && md->pending_n == 2) {
      /* "**" at EOF - treat as bold toggle. */
      toggle_bold(md);
    } else if (p0 == '*' && md->pending_n == 1 && md->italic) {
      /* Lone '*' closing an open italic span. */
      toggle_italic(md);
    } else if (p0 == '_' && md->italic) {
      /* Lone '_' closing an open italic span. */
      toggle_italic(md);
    } else if (p0 == '`' && md->pending_n == 1 && md->code) {
      /* Lone '`' closing an open inline-code span. */
      toggle_code(md);
    } else if (p0 == '`' && md->pending_n == 2) {
      /* "``" at EOF is rare - in real streams the producer won't
       * leave a fence half-open. Fall back to literal emission
       * instead of guessing. */
      raw_n(md, md->pending, md->pending_n);
    } else {
      /* Everything else: emit literally. Includes lone '*'/'_'/'`'
       * when the corresponding span is not open, lone '#', lone
       * '\\', and any partial fence we never closed. */
      raw_n(md, md->pending, md->pending_n);
    }
    md->pending_n = 0;
  }
  /* Close any open spans. Order matters only cosmetically - the
   * terminal processes SGR as a flat stack. We emit a full reset
   * instead of individual close codes so downstream chrome renders
   * cleanly even on terminals with buggy partial-SGR handling. */
  if (md->bold || md->italic || md->code || md->heading) {
    raw_n(md, "\x1b[0m", 4);
    md->bold = md->italic = md->code = md->heading = 0;
  }
  /* fence left open is an honest-to-god malformed stream; reset
   * state but don't emit anything - the raw bytes already hit the
   * sink faithfully. */
  md->fence          = 0;
  md->bol            = 1;
  md->utf8_pending_n = 0;
}

void xMdReset(xMd *md) {
  md->pending_n = 0;
  if (md->bold || md->italic || md->code || md->heading) {
    raw_n(md, "\x1b[0m", 4);
  }
  md->bold = md->italic = md->code = md->heading = md->fence = 0;
  md->bol                                                    = 1;
  md->utf8_pending_n                                         = 0;
}
