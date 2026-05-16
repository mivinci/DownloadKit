/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * banner.cpp - startup banner rendering.
 *
 * See banner.h for intent. This TU owns:
 *
 *   - the curated Logo table (variable-height ASCII art + width);
 *   - the ASCII soft-wrap helper used by the degraded-mode hint;
 *   - the single banner_print() entry, which picks a logo at random,
 *     then lays out title + body + tips in a 72-col bordered box.
 *
 * All logos live here as string literals so the compiler can
 * (statically) vouch for their shape: every row of a given Logo is
 * exactly `w` bytes wide, verified with static_assert at the
 * declaration site. Adding a new variant is a matter of dropping
 * a row-array of equal-length strings and adding an entry to the
 * kLogos table.
 *
 * The random pick uses std::rand() seeded once per process with
 * time()^getpid(). That's deliberately weak (the banner is not a
 * security surface) and avoids dragging in <random> machinery into
 * an otherwise tiny file. */

#include "banner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <vector>

/* ── Logo table ───────────────────────────────────────────────────
 *
 * Curated set of ASCII-art "moo" logos. Each row of a given Logo
 * is exactly `w` display cells wide; since the art is pure ASCII,
 * byte count == width. The %-Ns padding in banner_print relies on
 * this, so the static_asserts below are load-bearing.
 *
 * Both width (`w`) and height (`h`) vary per logo. The right-column
 * width in the box is computed as BOX_INNER - GAP_W - logo.w so the
 * frame still lands on 72 cells regardless of which logo was picked;
 * the rendered body height is max(logo.h, info_rows) so a taller
 * logo (e.g. the cowsay cow) just produces a taller body, and a
 * shorter logo is padded below with blank left-column cells so the
 * info rows on the right still appear. */
namespace {

struct Logo {
  const char *const *rows;
  int                h;
  int                w;
};

/* V1 — slant with square openings. The original hand-drawn logo;
 * kept first in the table for familiarity; appears with probability
 * 1/N like the rest. */
static const char *kV1[] = {
  "  __ _  ___  ___ ",
  " /  ' \\[_  \\[_  \\",
  " \\/_/_/[___/[___/",
};
static_assert(sizeof("  __ _  ___  ___ ") - 1 == 17, "V1 r0 width");
static_assert(sizeof(" /  ' \\[_  \\[_  \\") - 1 == 17, "V1 r1 width");
static_assert(sizeof(" \\/_/_/[___/[___/") - 1 == 17, "V1 r2 width");

/* V2 — slant with pipe openings. Same skeleton as V1 but the
 * square bracket is swapped for a vertical bar; reads more like a
 * stamped/engraved look. */
static const char *kV2[] = {
  "  __ _  ___  ___ ",
  " /  ' \\|_  \\|_  \\",
  " \\/_/_/|___/|___/",
};
static_assert(sizeof("  __ _  ___  ___ ") - 1 == 17, "V2 r0 width");
static_assert(sizeof(" /  ' \\|_  \\|_  \\") - 1 == 17, "V2 r1 width");
static_assert(sizeof(" \\/_/_/|___/|___/") - 1 == 17, "V2 r2 width");

/* V3 — slant "m" with dotted "o"s. The two o's are rendered as
 * .-. / `-' which reads as little circles without being symmetric
 * parentheses; the m stays in slant for continuity with V1/V2. */
static const char *kV3[] = {
  "  __ _   _    _  ",
  " /  ' \\ .-. .-.  ",
  " \\/_/_/ `-' `-'  ",
};
static_assert(sizeof("  __ _   _    _  ") - 1 == 17, "V3 r0 width");
static_assert(sizeof(" /  ' \\ .-. .-.  ") - 1 == 17, "V3 r1 width");
static_assert(sizeof(" \\/_/_/ `-' `-'  ") - 1 == 17, "V3 r2 width");

/* V4 — cowsay. Four rows, 19 cells wide. Every row is padded with
 * trailing spaces to the common width so the right-column info
 * (model / data_dir) still lines up under the same column. This
 * is the one logo that actually looks like a cow, which is the
 * whole point of "moo". */
static const char *kV4[] = {
  "\\   ^__^           ",
  " \\  (oo)\\_______   ",
  "    (__)\\       )\\/",
  "        ||----w |  ",
};
static_assert(sizeof("\\   ^__^           ") - 1 == 19, "V4 r0 width");
static_assert(sizeof(" \\  (oo)\\_______   ") - 1 == 19, "V4 r1 width");
static_assert(sizeof("    (__)\\       )\\/") - 1 == 19, "V4 r2 width");
static_assert(sizeof("        ||----w |  ") - 1 == 19, "V4 r3 width");

static const Logo kLogos[] = {
  {kV1, 3, 17},
  {kV2, 3, 17},
  {kV3, 3, 17},
  {kV4, 4, 19},
};
static const int kLogoCount = (int)(sizeof(kLogos) / sizeof(kLogos[0]));

/* Pick a logo from the table. Seeded once per process with a
 * time^pid mix (non-crypto, fine for a UI flourish). */
const Logo &pick_logo() {
  static bool seeded = false;
  if (!seeded) {
    std::srand((unsigned)std::time(nullptr) ^ (unsigned)getpid());
    seeded = true;
  }
  return kLogos[std::rand() % kLogoCount];
}

/* ── Banner text-wrap helper ──────────────────────────────────────
 *
 * Soft-wrap a pure-ASCII paragraph to a column width for drawing
 * inside the startup banner's bordered box. Break priority:
 *   1. space   — natural word break, preferred
 *   2. '/'     — next-best break point (long POSIX paths have
 *                plenty of these, so a deeply-nested data_dir
 *                doesn't overflow); break kept *after* the slash
 *                so the reader still sees the separator on the
 *                upper line.
 *   3. hard cut at `width` — last-resort fallback when a single
 *                token (e.g. a path component with no slashes)
 *                is longer than the column.
 *
 * Input must be pure ASCII (byte count == display width); the
 * banner's width accounting relies on that and Unicode here would
 * throw off the %-*s padding downstream. Empty input produces one
 * empty line so the caller can still emit a blank row and keep
 * vertical rhythm. */
std::vector<std::string> wrap(const std::string &text, size_t width) {
  std::vector<std::string> out;
  if (width == 0) {
    out.push_back(text);
    return out;
  }
  size_t i = 0, n = text.size();
  while (i < n) {
    if (n - i <= width) {
      out.push_back(text.substr(i));
      break;
    }
    size_t brk_space = std::string::npos;
    size_t brk_slash = std::string::npos;
    for (size_t j = 0; j < width; j++) {
      char c = text[i + j];
      if (c == ' ')
        brk_space = j;
      else if (c == '/')
        brk_slash = j;
    }
    if (brk_space != std::string::npos) {
      out.push_back(text.substr(i, brk_space));
      i += brk_space + 1;
    } else if (brk_slash != std::string::npos) {
      out.push_back(text.substr(i, brk_slash + 1));
      i += brk_slash + 1;
    } else {
      out.push_back(text.substr(i, width));
      i += width;
    }
  }
  if (out.empty()) out.push_back("");
  return out;
}

} // namespace

/* ── Entry point ──────────────────────────────────────────────────
 *
 * See banner.h for the layout description. Field widths:
 *
 *   │ <LOGO logo.w cols><2 cols gap><RIGHT w cols> │
 *
 * with BOX_INNER = 68 and full frame = 72. RIGHT_W adapts to the
 * picked logo so wider art shrinks the right column rather than
 * bursting the frame.
 *
 * Vertical layout of the logo strip: we render max(logo.h,
 * info_rows) rows. On rows where the logo has run out of art we
 * print `logo.w` spaces for the left field so the right column
 * still aligns. The info lines (model + data_dir, 2 rows in the
 * happy path; 0 rows in degraded mode) are anchored to the bottom
 * of that strip, so a tall logo sits above its captions rather
 * than leaving blank space below them. */
void banner_print(const char *version, const char *model_label, const char *tools_label,
                  const char *data_dir, int no_models) {
  const Logo &logo = pick_logo();

  enum {
    GAP_W     = 2,
    BOX_INNER = 68
  };
  const int LOGO_W  = logo.w;
  const int RIGHT_W = BOX_INNER - GAP_W - LOGO_W;

  /* Build the right-column strings for the logo strip. In the
   * happy path we emit 2 lines (model, data_dir); in degraded
   * mode none — the advisory block handles it. */
  std::vector<std::string> info;
  if (!no_models) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), "model=%s, tools=%s", model_label ? model_label : "?",
                  tools_label ? tools_label : "?");
    info.push_back(buf);
    std::snprintf(buf, sizeof(buf), "data_dir=%s", data_dir ? data_dir : "?");
    info.push_back(buf);
  }

  const int strip_h    = logo.h > (int)info.size() ? logo.h : (int)info.size();
  const int info_start = strip_h - (int)info.size(); /* bottom-anchored */

  /* Leading blank line: the parent shell's prompt sits right above
   * our first row, so without this gap the top border visually
   * collides with `$ moo` (or whatever PS1 trailed on). One row
   * of breathing room is enough and costs nothing. */
  std::printf("\n");

  /* Top border is 72 cells: "┌─ " (3) + "MOO " (4) + VERSION + " " (1)
   * + N*"─" + "┐" (1). Version is injected by CMake via MOO_VERSION
   * so the banner never drifts from the real build. */
  {
    const char *ver    = version ? version : "?";
    int         ver_w  = (int)std::strlen(ver);
    int         dashes = 72 - 3 - 4 - ver_w - 1 - 1;
    if (dashes < 0) dashes = 0;
    std::printf("\x1b[2m┌─ \x1b[22m\x1b[1mMOO %s\x1b[22m\x1b[2m ", ver);
    for (int i = 0; i < dashes; i++)
      std::printf("─");
    std::printf("┐\x1b[22m\n");
  }
  /* empty top padding row */
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");

  /* Logo strip. For each of `strip_h` rows: print the logo row (or
   * `logo.w` spaces once the logo has run out), a GAP_W gap, then
   * the corresponding info line left-padded to RIGHT_W. Info lines
   * are truncated to RIGHT_W by snprintf'ing through a fixed-size
   * buffer so a long model id or deep path can't blow the frame. */
  {
    char *rbuf = (char *)std::malloc((size_t)RIGHT_W + 1);
    if (!rbuf) return;
    for (int i = 0; i < strip_h; i++) {
      const char *lrow = (i < logo.h) ? logo.rows[i] : "";
      const char *itxt = "";
      if (i >= info_start && i - info_start < (int)info.size()) itxt = info[i - info_start].c_str();
      std::snprintf(rbuf, (size_t)RIGHT_W + 1, "%s", itxt);
      std::printf("\x1b[2m│\x1b[22m %-*s%*s%-*s \x1b[2m│\x1b[22m\n", LOGO_W, lrow, GAP_W, "",
                  RIGHT_W, rbuf);
    }
    std::free(rbuf);
  }

  /* blank separator before the degraded-mode hint / tips strip */
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");

  /* Degraded-mode hint block. Full-width wrapped paragraph placed
   * below the logo rather than compressed into the right column.
   * The 2-col left indent matches the tips strip and logo inset;
   * the wrap width is BOX_INNER - indent so first and continuation
   * lines align under the same column. Yellow attribute marks the
   * whole block as a single advisory. */
  if (no_models) {
    char hint_buf[4096];
    std::snprintf(hint_buf, sizeof(hint_buf),
                  "[!] no model is configured, edit %s/models.json "
                  "to enable chat",
                  data_dir ? data_dir : "?");
    const size_t indent     = 2;
    auto         hint_lines = wrap(hint_buf, BOX_INNER - indent);
    char         padded[BOX_INNER + 1];
    for (const auto &ln : hint_lines) {
      std::snprintf(padded, sizeof(padded), "  %s", ln.c_str());
      std::printf("\x1b[2m│\x1b[22m \x1b[33m%-*s\x1b[39m \x1b[2m│\x1b[22m\n", BOX_INNER, padded);
    }
    std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  }

  /* one-line tips strip (indent 2 cols to match logo inset) */
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "  Enter send   / commands   Ctrl-C cancel/exit   /help more");
  /* empty bottom padding row */
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  /* bottom: '└' + 70 '─' + '┘' = 72 */
  std::printf("\x1b[2m└"
              "──────────────────────────────────────────────────────────────────────"
              "┘\x1b[22m\n\n");
}
