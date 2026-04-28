/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ai_session.cpp - Streaming REPL driven by the full xai stack
 *                  (xAiAgent + xAiSession + xAiTool + xAiProvider).
 *
 * Unlike ai_openai.cpp (which drives the provider vtable directly for
 * end-to-end diagnostics), this demo is the canonical integration
 * path that user code should copy:
 *
 *   xAiProvider -> xAiAgent -> xAiSession
 *
 * The session hides the tool-call loop entirely: when the model asks
 * to call `get_time`, the session invokes our handler, folds the
 * result back into history, and submits another round on its own.
 * The REPL only sees streamed text plus a single on_done event per
 * user input.
 *
 * Usage:
 *   export LLM_API_URL="https://api.openai.com/v1"   # optional, no
 *                                                    # trailing slash
 *   export LLM_API_KEY="sk-xxx"
 *   export LLM_MODEL="gpt-4o"                        # optional
 *   ./ai_session [-d <path>]                         # default: cwd
 */

#include "xbase/backtrace.h"
#include <xai/agent.h>
#include <xai/message.h>
#include <xai/provider.h>
#include <xai/provider_openai.h>
#include <xai/session.h>
#include <xai/tool.h>
#include <xbase/event.h>
#include <xhttp/client.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <unistd.h>

/* ── Lightweight JSON field extractors ──────────────────────────────
 *
 * Tool handlers receive `in->u.tool_use.args_json` as a raw JSON
 * string produced by the model. xbase does not ship a JSON parser,
 * and pulling one in just for the demo is overkill — the model-side
 * arguments are tiny, flat objects and we only need three accessors:
 * string / int / bool, by key.
 *
 * These helpers are intentionally minimal: they don't handle nested
 * objects, arrays, unicode escapes, or numbers with exponents. The
 * tool JSON schemas below are authored so the model never produces
 * such shapes. If the model violates the schema, the handler simply
 * returns a tool_result with is_error=1 — the model will see the
 * error and retry or apologise.
 */

/* Skip whitespace. Returns new cursor. */
static const char *json_skip_ws(const char *p) {
  while (*p && std::isspace((unsigned char)*p))
    ++p;
  return p;
}

/* Find the value cursor for a top-level key in a JSON object.
 * Returns NULL if not found. Cursor points AT the value's first
 * non-whitespace char (`"`, digit, `t`/`f`, `{`, `[`, ...). */
static const char *json_find_value(const char *src, const char *key) {
  if (!src || !key) return nullptr;
  size_t      klen = std::strlen(key);
  const char *p    = src;
  /* Scan for `"<key>"` followed by `:`. Not bullet-proof (could
   * match inside a string value), but our schemas use short,
   * distinct keys so collisions don't happen in practice. */
  while (*p) {
    const char *q = std::strstr(p, "\"");
    if (!q) return nullptr;
    ++q; /* past opening quote */
    if (std::strncmp(q, key, klen) == 0 && q[klen] == '"') {
      const char *r = json_skip_ws(q + klen + 1);
      if (*r == ':') return json_skip_ws(r + 1);
    }
    /* advance past this key-or-string and keep scanning */
    p = std::strchr(q, '"');
    if (!p) return nullptr;
    ++p;
  }
  return nullptr;
}

/* Copy a JSON string value into `out` (NUL-terminated, truncated to
 * out_size-1). Returns true on success. Handles `\"`, `\\`, `\n`,
 * `\t`, `\r` escapes — enough for prose passed from the model. */
static bool json_find_string(const char *src, const char *key, char *out,
                             size_t out_size) {
  const char *p = json_find_value(src, key);
  if (!p || *p != '"' || out_size == 0) return false;
  ++p; /* skip opening quote */
  size_t n = 0;
  while (*p && *p != '"') {
    char c;
    if (*p == '\\' && p[1]) {
      switch (p[1]) {
      case '"':
        c = '"';
        break;
      case '\\':
        c = '\\';
        break;
      case '/':
        c = '/';
        break;
      case 'n':
        c = '\n';
        break;
      case 't':
        c = '\t';
        break;
      case 'r':
        c = '\r';
        break;
      case 'b':
        c = '\b';
        break;
      case 'f':
        c = '\f';
        break;
      default:
        c = p[1];
        break; /* pass through */
      }
      p += 2;
    } else {
      c = *p++;
    }
    if (n + 1 < out_size) out[n++] = c;
  }
  out[n] = '\0';
  return *p == '"';
}

/* Parse a JSON integer value. Returns true on success. */
static bool json_find_int(const char *src, const char *key, long *out) {
  const char *p = json_find_value(src, key);
  if (!p) return false;
  char *end = nullptr;
  long  v   = std::strtol(p, &end, 10);
  if (end == p) return false;
  *out = v;
  return true;
}

/* ── REPL state ─────────────────────────────────────────────────────── */
struct ReplCtx {
  xEventLoop loop            = nullptr;
  bool       saw_first_delta = false;
  bool       in_thinking     = false; /* currently streaming thinking? */
  size_t     reply_bytes     = 0;
  int        total_tokens    = 0;     /* cumulative across all rounds */
  size_t     budget_limit    = 0;     /* from last GatePassed event */
  size_t     budget_remaining = 0;    /* from last GatePassed event */
  double     budget_factor   = 1.0;   /* EWMA calibrator factor */
  size_t     budget_samples  = 0;     /* calibrator observation count */
  size_t     budget_estimated = 0;    /* calibrated pre-submit estimate */
  int        last_actual_prompt = -1; /* provider-reported first-round prompt_tokens */
};

/* ── Tools ──────────────────────────────────────────────────────────
 *
 * Every handler below follows the same shape:
 *   1. Parse `in->u.tool_use.args_json` into local variables.
 *   2. Compute the result into a thread-local buffer.
 *   3. Fill `out->u.tool_result` — session.c copies the bytes before
 *      this function returns, so the thread-local storage is safe.
 *
 * `out->u.tool_result.id` is left NULL: the session layer substitutes
 * the matching tool_use id when it folds the result back into
 * history. This keeps handlers agnostic to how calls are threaded.
 *
 * Error reporting: on bad input (parse failures, schema violations,
 * runtime errors like divide-by-zero), the handler sets is_error=1
 * and writes a short human-readable message into the same buffer.
 * The model sees that as a failed tool_result and will typically
 * apologise or retry with corrected args. Returning a non-Ok xErrno
 * would surface instead as on_error in the session, ending the run —
 * we only want that for genuinely unrecoverable failures (OOM), not
 * for bad args. */

/* Tool: get_time — current UTC time in ISO-8601. No arguments. */
static xErrno tool_get_time(const xAiContent *in, xAiContent *out, void *ud) {
  (void)in;
  (void)ud;

  static thread_local char buf[64];
  std::time_t              now = std::time(nullptr);
  std::tm                  tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &now);
#else
  gmtime_r(&now, &tm_utc);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

  out->type                     = xAiContentType_ToolResult;
  out->u.tool_result.id         = nullptr;
  out->u.tool_result.output     = buf;
  out->u.tool_result.output_len = std::strlen(buf);
  out->u.tool_result.is_error   = 0;
  return xErrno_Ok;
}

/* ── Tool: calculator ────────────────────────────────────────────────
 *
 * Shunting-yard over `double`. Supports `+ - * / %`, parens, and
 * unary minus. Integer literals and decimals are both accepted; the
 * result is printed as an integer when it's whole, otherwise with
 * %g so trailing zeros don't pollute the answer.
 *
 * We use two small stacks (values and operators). Parentheses are
 * just high-priority markers that swallow everything until the
 * matching `(`. Operator precedence follows the usual C convention
 * (`* / %` bind tighter than `+ -`). Division by zero is surfaced
 * as a tool error — the model can then apologise. */
static int calc_prec(char op) {
  switch (op) {
  case '+':
  case '-':
    return 1;
  case '*':
  case '/':
  case '%':
    return 2;
  case 'u':
    return 3; /* unary minus, internal token */
  default:
    return 0;
  }
}

static bool calc_apply(char op, double *vals, size_t *nv, const char **err) {
  if (op == 'u') {
    if (*nv < 1) {
      *err = "unary minus without operand";
      return false;
    }
    vals[*nv - 1] = -vals[*nv - 1];
    return true;
  }
  if (*nv < 2) {
    *err = "binary operator without two operands";
    return false;
  }
  double b = vals[--(*nv)];
  double a = vals[--(*nv)];
  double r = 0;
  switch (op) {
  case '+':
    r = a + b;
    break;
  case '-':
    r = a - b;
    break;
  case '*':
    r = a * b;
    break;
  case '/':
    if (b == 0) {
      *err = "division by zero";
      return false;
    }
    r = a / b;
    break;
  case '%':
    if (b == 0) {
      *err = "modulo by zero";
      return false;
    }
    /* fmod on doubles so both int and fp inputs work */
    r = std::fmod(a, b);
    break;
  default:
    *err = "unknown operator";
    return false;
  }
  vals[(*nv)++] = r;
  return true;
}

static bool calc_eval(const char *expr, double *out, const char **err) {
  /* Plenty of headroom for any expression the model will plausibly
   * emit — 128 tokens each side. */
  enum {
    STACK_CAP = 128
  };
  double vals[STACK_CAP];
  char   ops[STACK_CAP];
  size_t nv = 0, no = 0;
  bool   want_value = true; /* next token should be a value / unary? */

  const char *p = expr;
  while (*p) {
    if (std::isspace((unsigned char)*p)) {
      ++p;
      continue;
    }

    if (std::isdigit((unsigned char)*p) || *p == '.') {
      char  *end = nullptr;
      double v   = std::strtod(p, &end);
      if (end == p) {
        *err = "bad number";
        return false;
      }
      if (nv == STACK_CAP) {
        *err = "expression too deep";
        return false;
      }
      vals[nv++] = v;
      p          = end;
      want_value = false;
      continue;
    }

    if (*p == '(') {
      if (no == STACK_CAP) {
        *err = "expression too deep";
        return false;
      }
      ops[no++] = '(';
      ++p;
      want_value = true;
      continue;
    }
    if (*p == ')') {
      while (no && ops[no - 1] != '(') {
        if (!calc_apply(ops[--no], vals, &nv, err)) return false;
      }
      if (!no) {
        *err = "unbalanced ')'";
        return false;
      }
      --no; /* pop '(' */
      ++p;
      want_value = false;
      continue;
    }

    char op = *p;
    if (op == '+' || op == '-' || op == '*' || op == '/' || op == '%') {
      /* Disambiguate unary minus: '-' in a context expecting a value
       * is unary (token 'u'). Unary plus is a no-op, so just skip. */
      if (want_value) {
        if (op == '-') {
          if (no == STACK_CAP) {
            *err = "expression too deep";
            return false;
          }
          ops[no++] = 'u';
          ++p;
          /* still expecting a value */
          continue;
        }
        if (op == '+') {
          ++p;
          continue;
        }
        *err = "operator in value position";
        return false;
      }
      /* Binary op: pop anything of equal-or-higher precedence. */
      while (no && ops[no - 1] != '(' &&
             calc_prec(ops[no - 1]) >= calc_prec(op)) {
        if (!calc_apply(ops[--no], vals, &nv, err)) return false;
      }
      if (no == STACK_CAP) {
        *err = "expression too deep";
        return false;
      }
      ops[no++] = op;
      ++p;
      want_value = true;
      continue;
    }

    *err = "unexpected character";
    return false;
  }

  while (no) {
    if (ops[no - 1] == '(') {
      *err = "unbalanced '('";
      return false;
    }
    if (!calc_apply(ops[--no], vals, &nv, err)) return false;
  }
  if (nv != 1) {
    *err = "missing operand";
    return false;
  }
  *out = vals[0];
  return true;
}

static xErrno tool_calculator(const xAiContent *in, xAiContent *out, void *ud) {
  (void)ud;
  static thread_local char buf[256];

  char expr[256];
  if (!json_find_string(in->u.tool_use.args_json, "expr", expr, sizeof(expr))) {
    std::snprintf(buf, sizeof(buf), "missing or invalid 'expr' argument");
    out->type                     = xAiContentType_ToolResult;
    out->u.tool_result.id         = nullptr;
    out->u.tool_result.output     = buf;
    out->u.tool_result.output_len = std::strlen(buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  double      result = 0;
  const char *err    = nullptr;
  bool        ok     = calc_eval(expr, &result, &err);

  if (!ok) {
    std::snprintf(buf, sizeof(buf), "calc error: %s (expr=%s)",
                  err ? err : "unknown", expr);
    out->type                     = xAiContentType_ToolResult;
    out->u.tool_result.id         = nullptr;
    out->u.tool_result.output     = buf;
    out->u.tool_result.output_len = std::strlen(buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  /* Whole number? Print as long long to avoid "42.000000". */
  if (result == (double)(long long)result && result > -1e18 && result < 1e18) {
    std::snprintf(buf, sizeof(buf), "%lld", (long long)result);
  } else {
    std::snprintf(buf, sizeof(buf), "%.12g", result);
  }
  out->type                     = xAiContentType_ToolResult;
  out->u.tool_result.id         = nullptr;
  out->u.tool_result.output     = buf;
  out->u.tool_result.output_len = std::strlen(buf);
  out->u.tool_result.is_error   = 0;
  return xErrno_Ok;
}

/* ── Tool: random_int ────────────────────────────────────────────────
 *
 * Return a uniform integer in [min, max]. Uses a process-wide mt19937
 * seeded from std::random_device on first call; threading is not a
 * concern here because the demo session runs single-threaded on the
 * event loop. */
static xErrno tool_random_int(const xAiContent *in, xAiContent *out, void *ud) {
  (void)ud;
  static thread_local char buf[64];

  long lo = 0, hi = 0;
  bool got_lo = json_find_int(in->u.tool_use.args_json, "min", &lo);
  bool got_hi = json_find_int(in->u.tool_use.args_json, "max", &hi);
  if (!got_lo || !got_hi) {
    std::snprintf(buf, sizeof(buf), "missing or invalid 'min'/'max' arguments");
    out->type                     = xAiContentType_ToolResult;
    out->u.tool_result.id         = nullptr;
    out->u.tool_result.output     = buf;
    out->u.tool_result.output_len = std::strlen(buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }
  if (lo > hi) {
    std::snprintf(buf, sizeof(buf), "invalid range: min (%ld) > max (%ld)", lo,
                  hi);
    out->type                     = xAiContentType_ToolResult;
    out->u.tool_result.id         = nullptr;
    out->u.tool_result.output     = buf;
    out->u.tool_result.output_len = std::strlen(buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  /* Lazy-init on first call. uniform_int_distribution<long> is
   * inclusive on both ends, which matches the schema's contract. */
  static std::mt19937 *rng = nullptr;
  if (!rng) {
    std::random_device rd;
    rng = new std::mt19937(rd());
  }
  std::uniform_int_distribution<long> dist(lo, hi);
  long                                v = dist(*rng);

  std::snprintf(buf, sizeof(buf), "%ld", v);
  out->type                     = xAiContentType_ToolResult;
  out->u.tool_result.id         = nullptr;
  out->u.tool_result.output     = buf;
  out->u.tool_result.output_len = std::strlen(buf);
  out->u.tool_result.is_error   = 0;
  return xErrno_Ok;
}

/* ── Tool: wordcount ─────────────────────────────────────────────────
 *
 * Return char / word / line counts for a text blob. "Words" are
 * runs of non-whitespace (UTF-8 aware at the ASCII-whitespace level
 * only — adequate for a demo). The output is a tiny JSON so the
 * model can lift individual fields if it wants ("how many words?"). */
static xErrno tool_wordcount(const xAiContent *in, xAiContent *out, void *ud) {
  (void)ud;
  static thread_local char buf[128];
  static thread_local char text[4096];

  if (!json_find_string(in->u.tool_use.args_json, "text", text, sizeof(text))) {
    std::snprintf(buf, sizeof(buf), "missing or invalid 'text' argument");
    out->type                     = xAiContentType_ToolResult;
    out->u.tool_result.id         = nullptr;
    out->u.tool_result.output     = buf;
    out->u.tool_result.output_len = std::strlen(buf);
    out->u.tool_result.is_error   = 1;
    return xErrno_Ok;
  }

  size_t chars   = std::strlen(text);
  size_t lines   = 0;
  size_t words   = 0;
  bool   in_word = false;
  for (const char *p = text; *p; ++p) {
    if (*p == '\n') ++lines;
    if (std::isspace((unsigned char)*p)) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++words;
    }
  }
  /* If text isn't empty and doesn't end with '\n', count the final
   * line too. This matches `wc -l` only if the input is newline-
   * terminated; we prefer the intuitive "lines of text you see". */
  if (chars > 0 && text[chars - 1] != '\n') ++lines;

  std::snprintf(buf, sizeof(buf), "{\"chars\":%zu,\"words\":%zu,\"lines\":%zu}",
                chars, words, lines);
  out->type                     = xAiContentType_ToolResult;
  out->u.tool_result.id         = nullptr;
  out->u.tool_result.output     = buf;
  out->u.tool_result.output_len = std::strlen(buf);
  out->u.tool_result.is_error   = 0;
  return xErrno_Ok;
}

/* ── Session callbacks ──────────────────────────────────────────────── */

/* Close an open thinking block: reset SGR (`\x1b[0m`), newline, AND
 * emit one blank line so whatever follows (final text, [tool],
 * [done], ...) has visual breathing room. The trailing blank is
 * important when thinking ends with a sentence that looks like a
 * reply ("简短回复：..." etc) — without it the eye merges the faint
 * scratchpad into the bright answer. Must be called before printing
 * anything that shouldn't inherit faint style. No-op if no thinking
 * block is open, so it's safe to sprinkle liberally. */
static void end_thinking(ReplCtx *ctx) {
  if (!ctx->in_thinking) return;
  std::fputs("\x1b[0m\n\n", stdout);
  ctx->in_thinking = false;
}

static void on_text(xAiSession sess, const char *chunk, size_t len, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Close the thinking block (if any) before the visible reply
   * starts, so they don't visually merge AND the terminal doesn't
   * stay in faint mode for the rest of the output. */
  if (ctx->in_thinking) {
    end_thinking(ctx);
    ctx->saw_first_delta = true;
  } else if (!ctx->saw_first_delta) {
    std::putchar('\n');
    ctx->saw_first_delta = true;
  }
  std::fwrite(chunk, 1, len, stdout);
  std::fflush(stdout);
  ctx->reply_bytes += len;
}

/* Thinking stream: dim + prefix so it's obviously "model scratchpad"
 * and not the final answer. ANSI 2 = faint; most modern terminals
 * honour it (including macOS Terminal and iTerm2). On the rare
 * terminal that doesn't, the `[thinking]` prefix still telegraphs
 * intent. */
static void on_thinking(xAiSession sess, const char *chunk, size_t len,
                        void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  if (!ctx->in_thinking) {
    /* Open a new thinking block on its own line. */
    std::fputs("\x1b[2m[thinking] ", stdout);
    ctx->in_thinking = true;
  }
  std::fwrite(chunk, 1, len, stdout);
  std::fflush(stdout);
}

static void on_tool(xAiSession sess, const char *tool_name, int started,
                    void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* [tool] frames are chrome — render faint, same as
   * [thinking]/[done]. Node placement rules:
   *   - After thinking: end_thinking() already emitted `\n\n`, so
   *     just print the line (no leading newline from us).
   *   - Otherwise (start of run, or after text): print one blank
   *     line first so [tool] doesn't glue to whatever was above. */
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  if (!after_thinking) std::putchar('\n');
  std::printf("\x1b[2m[tool] %s %s\x1b[0m\n", tool_name ? tool_name : "(null)",
              started ? "starting" : "finished\n");
  std::fflush(stdout);
}

static const char *done_reason_name(xAiDoneReason r) {
  switch (r) {
  case xAiDoneReason_Completed:
    return "completed";
  case xAiDoneReason_MaxTurns:
    return "max_turns";
  case xAiDoneReason_PromptTooLong:
    return "prompt_too_long";
  case xAiDoneReason_Aborted:
    return "aborted";
  case xAiDoneReason_ModelError:
    return "model_error";
  case xAiDoneReason_ToolError:
    return "tool_error";
  case xAiDoneReason_Stopped:
    return "stopped";
  }
  return "?";
}

static void on_done(xAiSession sess, xAiDoneReason reason,
                    const xAiUsage *usage, void *ud) {
  (void)sess;
  auto *ctx            = static_cast<ReplCtx *>(ud);
  bool  after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  /* [done] is chrome — render the whole line faint so it recedes and
   * the model's answer above stays visually primary. Extra blank
   * line after so the next `> ` prompt isn't glued to the status. */
  if (!after_thinking) std::putchar('\n');
  std::fputs("\x1b[2m", stdout);
  std::printf("\n[done] reason=%s reply_bytes=%zu", done_reason_name(reason),
              ctx->reply_bytes);
  /* Token accounting. prompt_tokens is the maximum across all
   * rounds (each round reports the full input size the provider
   * saw, so the last round's value is the total). completion_tokens
   * and total_tokens are additive per round. The provider fills -1
   * for fields it couldn't parse; we hide those so the line stays
   * clean for servers that only report a subset. A NULL usage means
   * the server never sent a usage object — rare in practice
   * (moonshot, openai, deepseek all support
   * stream_options.include_usage). */
  if (usage) {
    std::printf(" tokens=");
    if (usage->prompt_tokens >= 0) {
      std::printf("%d", usage->prompt_tokens);
    } else {
      std::printf("?");
    }
    std::printf("/");
    if (usage->completion_tokens >= 0) {
      std::printf("%d", usage->completion_tokens);
    } else {
      std::printf("?");
    }
    if (usage->total_tokens >= 0) {
      ctx->total_tokens += usage->total_tokens;
      std::printf(" total=%d", ctx->total_tokens);
    }
  }
  /* Context-budget snapshot. budget_remaining and budget_limit
   * are populated by the GatePassed event that fires at the start
   * of every round (before the Query is submitted). Displaying
   * remaining/limit gives the user a real-time view of how much
   * context headroom is left before the budget gate would trigger
   * TruncateOldest or SummarizeOldest. A remaining of 0 means
   * the very next input is likely to hit the cap.
   * calibrator_factor is the EWMA-smoothed multiplier that
   * bytes/4 gets scaled by; it starts at 1.0 and drifts toward
   * (actual_prompt_tokens / estimated_prompt_tokens). samples
   * is the count of accepted observations. est is the calibrated
   * pre-submit estimate for this round. */
  if (ctx->budget_limit > 0) {
    std::printf(" budget=%zu/%zu %.3fx samples=%zu est=%zu",
                ctx->budget_remaining, ctx->budget_limit,
                ctx->budget_factor, ctx->budget_samples,
                ctx->budget_estimated);
    if (ctx->last_actual_prompt >= 0) {
      std::printf(" actual=%d", ctx->last_actual_prompt);
    }
  }
  std::fputs("\x1b[0m\n\n", stdout);
  std::fflush(stdout);
  xEventLoopStop(ctx->loop);
}

static void on_error(xAiSession sess, xErrno err, const char *msg, void *ud) {
  (void)sess;
  auto *ctx = static_cast<ReplCtx *>(ud);
  /* Same SGR hygiene as on_done — error might fire mid-thinking.
   * Errors are the one piece of chrome that should NOT recede — use
   * bold red (`\x1b[1;31m`) instead of faint so the user notices the
   * run failed at a glance. */
  bool after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  if (!after_thinking) std::fputc('\n', stderr);
  std::fprintf(stderr, "\x1b[1;31m[error] errno=%d msg=%s\x1b[0m\n", (int)err,
               msg ? msg : "(none)");
  /* Surface the budget gate explicitly. PromptTooLong is the one
   * errno most likely to surprise a demo user ("I didn't do
   * anything weird, why did my innocuous follow-up get rejected?")
   * — it means either the rolling history plus the incoming
   * message overflowed sconf.budget.max_tokens with no room to
   * trim below keep_recent_turns, or the incoming message alone
   * is bigger than the cap. The fix is almost always "raise the
   * cap" for a calibrator demo; production callers would
   * typically switch to SummarizeOldest or a Callback policy. */
  if (err == xErrno_PromptTooLong) {
    std::fprintf(stderr, "\x1b[1;31m        hit budget cap — raise "
                         "sconf.budget.max_tokens or lower "
                         "keep_recent_turns\x1b[0m\n");
  }
  std::fputc('\n', stderr);
  std::fflush(stderr);
  xEventLoopStop(ctx->loop);
}

/* ── Budget-event callback ────────────────────────────────────────────
 *
 * Registered via sconf.budget.on_budget_event so the REPL user can
 * observe the SummarizeOldest / TruncateOldest lifecycle in real time.
 * All three events are informational; ignoring them doesn't change
 * session behaviour, but surfacing them makes the budget demo much
 * easier to follow — the user sees why a subsequent xAiSessionInput
 * returned Busy (Compacting) and knows when to retry (CompactDone). */
static void on_budget_event(xAiSession sess, xAiBudgetEvent event,
                            const void *info, void *ud) {
  (void)sess;
  auto *ctx            = static_cast<ReplCtx *>(ud);
  bool  after_thinking = ctx->in_thinking;
  end_thinking(ctx);
  /* Budget events are chrome — render faint, same as [tool]/[done]. */
  if (!after_thinking) std::putchar('\n');
  std::fputs("\x1b[2m", stdout);
  switch (event) {
  case xAiBudgetEvent_Compacting: {
    auto *ci = static_cast<const xAiBudgetCompactInfo *>(info);
    std::printf("[budget] compacting %zu old entries...",
                ci ? ci->entries_compacted : 0);
    break;
  }
  case xAiBudgetEvent_CompactDone: {
    auto *cdi = static_cast<const xAiBudgetCompactDoneInfo *>(info);
    if (cdi && cdi->summary_ok) {
      std::printf("[budget] compact done — summary %zu tokens, "
                  "%zu entries affected",
                  cdi->summary_tokens, cdi->entries_affected);
    } else {
      std::printf(
        "[budget] compact degraded to truncate — %zu entries affected",
        cdi ? cdi->entries_affected : 0);
    }
    /* Compact is done — stop the event loop so the REPL's
     * xEventLoopRun returns and the auto-retry after Busy
     * can proceed. Without this the loop blocks forever
     * because the compact on_done path does NOT call
     * xEventLoopStop (it's an internal operation, not a
     * user-visible Query completion). */
    xEventLoopStop(ctx->loop);
    break;
  }
  case xAiBudgetEvent_Truncated: {
    auto *ti = static_cast<const xAiBudgetTruncateInfo *>(info);
    std::printf("[budget] truncated %zu old entries",
                ti ? ti->entries_removed : 0);
    break;
  }
  case xAiBudgetEvent_GatePassed: {
    auto *gi = static_cast<const xAiBudgetGateInfo *>(info);
    if (gi) {
      ctx->budget_limit     = gi->limit;
      ctx->budget_remaining = gi->remaining;
      ctx->budget_factor    = gi->calibrator_factor;
      ctx->budget_samples   = gi->calibrator_samples;
      ctx->budget_estimated = gi->estimated;
      ctx->last_actual_prompt = gi->last_first_round_prompt_tokens;
      std::printf("[budget] gate passed — remaining %zu/%zu tokens",
                  gi->remaining, gi->limit);
    }
    break;
  }
  }
  std::fputs("\x1b[0m\n", stdout);
  std::fflush(stdout);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  /* ── Parse command-line options ─────────────────────────────────── */
  int         opt;
  const char *data_dir_arg = nullptr;
  while ((opt = getopt(argc, argv, "d:h")) != -1) {
    switch (opt) {
    case 'd':
      data_dir_arg = optarg;
      break;
    case 'h':
    default:
      std::fprintf(stderr, "Usage: %s [-d <path>]\n", argv[0]);
      return 1;
    }
  }

  /* Default data_dir to the current working directory. */
  char        cwd_buf[4096];
  const char *data_dir = data_dir_arg;
  if (!data_dir) {
    if (getcwd(cwd_buf, sizeof(cwd_buf))) {
      data_dir = cwd_buf;
    } else {
      data_dir = ".";
    }
  }

  const char *api_url = std::getenv("LLM_API_URL");
  const char *api_key = std::getenv("LLM_API_KEY");
  const char *model   = std::getenv("LLM_MODEL");

  if (!api_key) {
    std::fprintf(stderr, "Please set at least LLM_API_KEY:\n"
                         "  export LLM_API_KEY=\"sk-xxx\"\n"
                         "  export LLM_API_URL=\"https://api.openai.com/v1\"  "
                         "(optional)\n"
                         "  export LLM_MODEL=\"gpt-4o\"                       "
                         "(optional)\n");
    return 1;
  }
  if (!model || model[0] == '\0') model = "gpt-4o";

  /* ── Event loop + HTTP client ───────────────────────────────────── */
  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    std::fprintf(stderr, "failed to create event loop\n");
    return 1;
  }
  xHttpClient http = xHttpClientCreate(loop, nullptr);
  if (!http) {
    std::fprintf(stderr, "failed to create http client\n");
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Provider ───────────────────────────────────────────────────── */
  xAiOpenAIConf pconf;
  std::memset(&pconf, 0, sizeof(pconf));
  pconf.api_key       = api_key;
  pconf.base_url      = api_url;
  pconf.default_model = model;
  pconf.timeout_ms    = 60000;

  xAiProvider pvd = xAiProviderOpenAICreate(loop, http, &pconf);
  if (!pvd) {
    std::fprintf(stderr, "failed to create OpenAI provider\n");
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Tools ───────────────────────────────────────────────────────────
   *
   * Each xAiToolCreate call returns a handle; xAiAgentConf wants an
   * array of handle pointers (const xAiTool **), so we collect the
   * handles first and then take their addresses. A single helper
   * keeps the error-path bail-out linear — if any creation fails we
   * destroy the ones already built and abort. */

  struct ToolSpec {
    const char        *name;
    const char        *description;
    const char        *schema;
    xAiToolHandlerFunc handler;
  };
  const ToolSpec specs[] = {
    {
      "get_time",
      "Return the current UTC time in ISO-8601 format. Takes no "
      "arguments.",
      "{\"type\":\"object\",\"properties\":{},"
      "\"additionalProperties\":false}",
      tool_get_time,
    },
    {
      "calculator",
      "Evaluate a basic arithmetic expression over numbers. "
      "Supports + - * / %, parentheses, and unary minus. Returns "
      "the numeric result as a string, or an error message when "
      "the expression is malformed or divides by zero.",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"expr\":{\"type\":\"string\","
      "\"description\":\"arithmetic expression, e.g. '1+2*3'\"}"
      "},"
      "\"required\":[\"expr\"],"
      "\"additionalProperties\":false}",
      tool_calculator,
    },
    {
      "random_int",
      "Return a uniform pseudo-random integer in the inclusive "
      "range [min, max].",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"min\":{\"type\":\"integer\"},"
      "\"max\":{\"type\":\"integer\"}"
      "},"
      "\"required\":[\"min\",\"max\"],"
      "\"additionalProperties\":false}",
      tool_random_int,
    },
    {
      "wordcount",
      "Count characters, words, and lines in a piece of text. "
      "Returns a JSON object {\"chars\":N,\"words\":N,\"lines\":N}.",
      "{\"type\":\"object\","
      "\"properties\":{"
      "\"text\":{\"type\":\"string\"}"
      "},"
      "\"required\":[\"text\"],"
      "\"additionalProperties\":false}",
      tool_wordcount,
    },
  };
  constexpr size_t N_TOOLS = sizeof(specs) / sizeof(specs[0]);

  xAiTool tool_handles[N_TOOLS] = {};
  for (size_t i = 0; i < N_TOOLS; ++i) {
    xAiToolConf tconf;
    std::memset(&tconf, 0, sizeof(tconf));
    tconf.name        = specs[i].name;
    tconf.description = specs[i].description;
    tconf.json_schema = specs[i].schema;
    tconf.handler     = specs[i].handler;
    tool_handles[i]   = xAiToolCreate(&tconf);
    if (!tool_handles[i]) {
      std::fprintf(stderr, "failed to create tool '%s'\n", specs[i].name);
      for (size_t j = 0; j < i; ++j)
        xAiToolDestroy(tool_handles[j]);
      xAiProviderDestroy(pvd);
      xHttpClientDestroy(http);
      xEventLoopDestroy(loop);
      return 1;
    }
  }

  /* xAiAgentConf::tools is `const xAiTool **` (array of handle
   * pointers, not an array of handles); collect addresses. */
  const xAiTool *tool_ptrs[N_TOOLS];
  for (size_t i = 0; i < N_TOOLS; ++i)
    tool_ptrs[i] = &tool_handles[i];
  /* ── Session config (agent's default session) ──────────────────────
   *
   * Instead of creating a session manually and managing its
   * lifecycle, we set default_session_conf on the agent so it
   * creates a built-in default session at construction time.
   * The session is retrieved via xAiAgentDefaultSession() and
   * is destroyed automatically by xAiAgentDestroy(). */
  ReplCtx ctx;
  ctx.loop = loop;

  xAiSessionConf sconf;
  std::memset(&sconf, 0, sizeof(sconf));
  sconf.cbs.on_text     = on_text;
  sconf.cbs.on_thinking = on_thinking;
  sconf.cbs.on_tool     = on_tool;
  sconf.cbs.on_done     = on_done;
  sconf.cbs.on_error    = on_error;
  sconf.cbs.user_data   = &ctx;

  /* Opt into the structured budget pipeline so the calibrator
   * actually runs. Without a non-Disabled policy the gate short-
   * circuits, last_prompt_estimate stays zero, and on_done's
   * calibrator update bails out — factor would forever read 1.0
   * and samples 0, defeating the whole point of this demo.
   *
   * 8192 was picked empirically: large enough that a single
   * long-form answer (think: a derivation with multi-paragraph
   * reasoning) plus the floor pinned by keep_recent_turns won't
   * trip the gate on turn #2, but small enough that a handful of
   * sustained turns will eventually push the rolling history
   * past the cap and exercise TruncateOldest. keep_recent_turns
   * =2 is the floor — the current user turn and the immediately
   * prior assistant turn are never discarded, so the model keeps
   * local context even when the trimmer fires. If you shrink
   * max_tokens below ~4096 expect xErrno_PromptTooLong (which the
   * REPL and on_error both surface with a hint line below), and
   * see session.c's keep_recent_turns floor logic for why. */
  sconf.budget.policy            = xAiBudgetPolicy_Auto;
  sconf.budget.max_tokens        = 2048;
  sconf.budget.keep_recent_turns = 2;
  sconf.budget.on_budget_event   = on_budget_event;
  sconf.budget.budget_event_ud   = &ctx;

  /* ── Agent ──────────────────────────────────────────────────────── */
  xAiAgentConf aconf;
  std::memset(&aconf, 0, sizeof(aconf));
  aconf.loop     = loop;
  aconf.provider = pvd;
  aconf.model    = model;
  aconf.system_prompt =
    "You are a concise assistant running on xKit's xai session "
    "demo. You have access to these tools:\n"
    "  - get_time: current UTC time (no args)\n"
    "  - calculator: evaluate arithmetic like '1+2*3'\n"
    "  - random_int: uniform int in [min, max]\n"
    "  - wordcount: count chars/words/lines of a text blob\n"
    "Use tools when they would produce a more accurate answer "
    "than guessing. You may chain multiple tool calls in a single "
    "turn. Keep replies short.";
  aconf.tools               = tool_ptrs;
  aconf.n_tools             = N_TOOLS;
  aconf.max_turns           = 8;
  aconf.agent_id            = "test";
  aconf.data_dir            = data_dir;
  aconf.default_session_conf = &sconf;

  xAiAgent agent = xAiAgentCreate(&aconf);
  if (!agent) {
    std::fprintf(stderr, "failed to create agent\n");
    for (size_t i = 0; i < N_TOOLS; ++i)
      xAiToolDestroy(tool_handles[i]);
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* Retrieve the agent's built-in default session — no manual
   * create/destroy needed. The session lives for the agent's
   * entire lifetime. */
  xAiSession sess = xAiAgentDefaultSession(agent);
  if (!sess) {
    std::fprintf(stderr, "agent has no default session\n");
    xAiAgentDestroy(agent);
    for (size_t i = 0; i < N_TOOLS; ++i)
      xAiToolDestroy(tool_handles[i]);
    xAiProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  std::printf("xai session REPL (model: %s)\n", model);
  std::printf("Type a message and press Enter. Ctrl-D or \"exit\" to quit.\n"
              "Registered tools:\n");
  for (size_t i = 0; i < N_TOOLS; ++i) {
    std::printf("  - %s\n", specs[i].name);
  }
  std::putchar('\n');

  char line[4096];
  while (true) {
    std::printf("> ");
    std::fflush(stdout);

    if (!std::fgets(line, sizeof(line), stdin)) break; /* EOF */

    size_t len = std::strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0) continue;
    if (std::strcmp(line, "exit") == 0 || std::strcmp(line, "quit") == 0) break;

    ctx.saw_first_delta = false;
    ctx.in_thinking     = false;
    ctx.reply_bytes     = 0;

    /* xAiMessageFromText creates a User-role borrow-view that points
     * at `line` via a thread-local content slot (see message.c).
     * xAiSessionInput duplicates every byte into session-owned
     * memory before it returns, so reusing `line` on the next
     * iteration is safe. */
    xAiMessage m   = xAiMessageFromText(line);
    xErrno     err = xAiSessionInput(sess, m);
    while (err == xErrno_Busy) {
      /* Busy from SummarizeOldest is expected — the on_budget_event
       * callback already printed "[budget] compacting ...". Run the
       * event loop so the compact Query can complete; on_budget_event
       * will print "[budget] compact done" and then we auto-retry
       * the user's input. If Busy is from a regular Query still in
       * flight (shouldn't happen in this single-flight REPL), the
       * loop returns immediately and we report it below.
       *
       * Loop (not single retry): after a compact that degraded to
       * TruncateOldest, the session may still be over budget, and
       * session_enforce_budget_ can launch another compact round.
       * Each Busy return means "an async operation is in flight";
       * we wait for it to finish and retry until the gate lets the
       * message through or returns a non-Busy error. */
      xEventLoopRun(loop);
      /* Compact done — session is idle now, retry the input. */
      err = xAiSessionInput(sess, m);
    }
    if (err != xErrno_Ok) {
      /* Synchronous rejection path: the gate fires before the
       * Query is even handed off, so on_error never runs. Mirror
       * the PromptTooLong hint from on_error here so the same
       * advice reaches users regardless of which path triggered.
       * Other errnos (Busy, InvalidState) have no budget-side
       * remedy, so they just print the bare code. */
      std::fprintf(stderr, "[error] input rejected (errno=%d)\n", (int)err);
      if (err == xErrno_PromptTooLong) {
        std::fprintf(stderr, "        hit budget cap — raise "
                             "sconf.budget.max_tokens or lower "
                             "keep_recent_turns\n");
      }
      continue;
    }

    xEventLoopRun(loop);
  }

  std::printf("\nBye!\n");

  /* No xAiSessionDestroy needed — the default session is owned
   * by the agent and destroyed automatically in xAiAgentDestroy. */
  xAiAgentDestroy(agent);
  for (size_t i = 0; i < N_TOOLS; ++i)
    xAiToolDestroy(tool_handles[i]);
  xAiProviderDestroy(pvd);
  xHttpClientDestroy(http);
  xEventLoopDestroy(loop);
  return 0;
}
