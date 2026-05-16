/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory_jsonl.c - Built-in file-backed xAgentMemory implementation
 *
 * Lays out one JSONL file per session_id under:
 *   {root_dir}/sessions/{session_id}/history.jsonl
 *
 * Append is a straight fopen("a") + fprintf; each line is one
 * xAgentSessionMsg serialised into a minimal, self-describing JSON
 * object (role / kind / payload + a wall-clock "ts" field).
 *
 * Retrieve reads the whole file into memory, splits on newlines,
 * keeps the last N lines that parse successfully, and materialises
 * each one as an xAgentSessionMsg pointing into a per-call arena
 * allocated alongside the entries array. Release frees the arena.
 *
 * Tradeoffs:
 *   - Reading the whole file on every retrieve is fine for L1-sized
 *     files (a few hundred KB at most per session). Very long-lived
 *     sessions that outgrow that should plug a smarter backend.
 *   - The line parser is tailored to the fields this module writes
 *     (role, kind, text / tool_use_* / tool_result_* / output /
 *     is_error). Any extra JSON keys are skipped. No dependency on
 *     cJSON so memory_jsonl stays in the hot path without adding
 *     a link dep beyond xbase.
 */

#include "memory_private.h"

#include <x/base/time.h> /* xWallMs */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── Backend state ─────────────────────────────────────────────────── */

#define XAGENT_MEMORY_JSONL_DEFAULT_MAX_ENTRIES 64

struct memory_jsonl_ {
  struct xAgentMemory_ base; /* MUST be first */
  char   *root_dir;          /* owned copy                            */
  size_t  default_max_entries;
};

/* ── Path helpers ─────────────────────────────────────────────────── */

/* Recursively create directories along @p path (like `mkdir -p`).
 * Only creates directory components — the trailing filename (if
 * any) is left alone. Returns 0 on success, -1 on error. */
static int mk_parent_dirs_(const char *path) {
  if (!path) return -1;

  const char *last_sep = strrchr(path, '/');
  if (!last_sep) return 0;

  size_t dir_len = (size_t)(last_sep - path);
  if (dir_len == 0) return 0;

  char *dir = (char *)malloc(dir_len + 1);
  if (!dir) return -1;
  memcpy(dir, path, dir_len);
  dir[dir_len] = '\0';

  for (size_t i = 1; i <= dir_len; i++) {
    if (dir[i] == '/' || dir[i] == '\0') {
      char saved = dir[i];
      dir[i]     = '\0';
      if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        free(dir);
        return -1;
      }
      dir[i] = saved;
    }
  }

  free(dir);
  return 0;
}

/* Build the per-session JSONL path. Returns a malloc'd string the
 * caller must free, or NULL on OOM / missing id. */
static char *build_path_(const struct memory_jsonl_ *b,
                         const char *session_id) {
  if (!b || !b->root_dir || !session_id) return NULL;

  size_t n = strlen(b->root_dir) + strlen("/sessions/") + strlen(session_id) +
             strlen("/history.jsonl") + 1;
  char *p = (char *)malloc(n);
  if (!p) return NULL;
  snprintf(p, n, "%s/sessions/%s/history.jsonl", b->root_dir, session_id);
  return p;
}

/* ── JSON escape (write side) ──────────────────────────────────────── */

/* Write one JSON-escaped chunk of opaque bytes into @p fp. Uses the
 * same escape rules as agent.c's legacy writer so both producers
 * emit byte-compatible files. */
static void write_json_str_(FILE *fp, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"') fputs("\\\"", fp);
    else if (c == '\\') fputs("\\\\", fp);
    else if (c == '\n') fputs("\\n", fp);
    else if (c == '\r') fputs("\\r", fp);
    else if (c == '\t') fputs("\\t", fp);
    else if (c < 0x20) fprintf(fp, "\\u%04x", c);
    else fputc((int)c, fp);
  }
}

static const char *role_to_str_(xAgentRole r) {
  switch (r) {
  case xAgentRole_System:    return "system";
  case xAgentRole_User:      return "user";
  case xAgentRole_Assistant: return "assistant";
  case xAgentRole_Tool:      return "tool";
  case xAgentRole_Summary:   return "summary";
  }
  return "system";
}

static const char *kind_to_str_(xAgentSessionEntryKind k) {
  switch (k) {
  case xAgentSessionEntryKind_Text: return "text";
  case xAgentSessionEntryKind_ToolUse: return "tool_use";
  case xAgentSessionEntryKind_ToolResult: return "tool_result";
  case xAgentSessionEntryKind_Thinking: return "thinking";
  }
  return "text";
}

/* Emit one JSONL line. The "ts" field records when the entry was
 * produced (user input, assistant stream, tool completion) — we
 * take it from @c created_at_ms if the caller already stamped it,
 * and only fall back to the current wall-clock when the field is
 * zero. Either way every persisted line ends up with a value so
 * future backends can sort / window by time without re-deriving
 * ordering from file position.
 *
 * Not used for current retrieval; older files lacking "ts" parse
 * just fine (parse_line_ tolerates unknown keys and accepts either
 * a plain number or any other scalar via parse_raw_value_into_arena_).
 */
static void write_msg_line_(FILE *fp, const xAgentSessionMsg *m) {
  uint64_t ts = m->created_at_ms != 0 ? m->created_at_ms : xWallMs();
  fprintf(fp, "{\"ts\":%llu,\"role\":\"%s\",\"kind\":\"%s\"",
          (unsigned long long)ts, role_to_str_(m->role), kind_to_str_(m->kind));

  if (m->kind == xAgentSessionEntryKind_Text ||
      m->kind == xAgentSessionEntryKind_Thinking) {
    if (m->text && m->text_len > 0) {
      fputs(",\"text\":\"", fp);
      write_json_str_(fp, m->text, m->text_len);
      fputc('"', fp);
    }
  } else if (m->kind == xAgentSessionEntryKind_ToolUse) {
    if (m->tool_use_id)
      fprintf(fp, ",\"tool_use_id\":\"%s\"", m->tool_use_id);
    if (m->tool_use_name)
      fprintf(fp, ",\"tool_use_name\":\"%s\"", m->tool_use_name);
    /* tool_use_args is already a JSON object string — emit raw. */
    if (m->tool_use_args)
      fprintf(fp, ",\"tool_use_args\":%s", m->tool_use_args);
  } else if (m->kind == xAgentSessionEntryKind_ToolResult) {
    if (m->tool_result_id)
      fprintf(fp, ",\"tool_result_id\":\"%s\"", m->tool_result_id);
    if (m->tool_result_is_error)
      fputs(",\"is_error\":true", fp);
    if (m->tool_result_output && m->tool_result_output_len > 0) {
      fputs(",\"output\":\"", fp);
      write_json_str_(fp, m->tool_result_output, m->tool_result_output_len);
      fputc('"', fp);
    }
  }

  fputs("}\n", fp);
}

/* ── JSON parse (read side) ────────────────────────────────────────
 *
 * Hand-rolled minimal parser tuned for the exact shape write_msg_line_
 * produces. It understands flat objects with string / bool values
 * and one raw-JSON value (tool_use_args). It is NOT a general JSON
 * parser — anything it doesn't recognise is skipped over. Input
 * lines that fail to parse cleanly are dropped from the result set.
 */

/* Skip ASCII whitespace. */
static const char *skip_ws_(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
  return p;
}

/* Match a literal keyword. Advances *pp on success. */
static int match_lit_(const char **pp, const char *end, const char *lit) {
  size_t n = strlen(lit);
  if ((size_t)(end - *pp) < n) return 0;
  if (memcmp(*pp, lit, n) != 0) return 0;
  *pp += n;
  return 1;
}

/* Parse a JSON string literal into a caller-provided arena. On
 * success *pp is advanced past the closing quote, the unescaped
 * bytes are copied into @p arena at offset *arena_used, and
 * *out_off / *out_len are set to point at the copy. */
static int parse_string_into_arena_(const char **pp, const char *end,
                                    char *arena, size_t arena_cap,
                                    size_t *arena_used, size_t *out_off,
                                    size_t *out_len) {
  const char *p = *pp;
  if (p >= end || *p != '"') return 0;
  p++;

  size_t off   = *arena_used;
  size_t start = off;

  while (p < end) {
    char c = *p++;
    if (c == '"') {
      /* Reserve one byte for the NUL terminator so consecutive
       * strings in the same arena don't clobber each other's
       * trailing '\0'. */
      if (off + 1 > arena_cap) return 0;
      arena[off] = '\0';
      *pp        = p;
      *arena_used = off + 1;
      *out_off    = start;
      *out_len    = off - start;
      return 1;
    }
    if (c == '\\') {
      if (p >= end) return 0;
      char esc = *p++;
      char replacement = 0;
      switch (esc) {
      case '"': replacement = '"'; break;
      case '\\': replacement = '\\'; break;
      case '/': replacement = '/'; break;
      case 'n': replacement = '\n'; break;
      case 'r': replacement = '\r'; break;
      case 't': replacement = '\t'; break;
      case 'b': replacement = '\b'; break;
      case 'f': replacement = '\f'; break;
      case 'u': {
        /* Accept \uXXXX; for simplicity, only emit the low byte for
         * code points <= 0x7F, else write a '?' placeholder. Real
         * UTF-8 re-encoding would bloat this module; upgrade when
         * a caller actually needs full unicode round-tripping. */
        if (end - p < 4) return 0;
        unsigned v = 0;
        for (int i = 0; i < 4; i++) {
          char h = p[i];
          unsigned digit;
          if (h >= '0' && h <= '9') digit = (unsigned)(h - '0');
          else if (h >= 'a' && h <= 'f') digit = (unsigned)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') digit = (unsigned)(h - 'A' + 10);
          else return 0;
          v = (v << 4) | digit;
        }
        p += 4;
        replacement = (v <= 0x7F) ? (char)v : '?';
        break;
      }
      default: return 0;
      }
      if (off + 1 > arena_cap) return 0;
      arena[off++] = replacement;
    } else {
      if (off + 1 > arena_cap) return 0;
      arena[off++] = c;
    }
  }
  return 0; /* unterminated */
}

/* Copy a raw JSON value (object / array / primitive) into the arena
 * as-is, NUL-terminated. Used for tool_use_args. Input starts at
 * the first non-ws character of the value. */
static int parse_raw_value_into_arena_(const char **pp, const char *end,
                                       char *arena, size_t arena_cap,
                                       size_t *arena_used, size_t *out_off,
                                       size_t *out_len) {
  const char *p    = *pp;
  const char *vstart = p;

  if (p >= end) return 0;
  char first = *p;
  if (first == '{' || first == '[') {
    char open_ch  = first;
    char close_ch = (first == '{') ? '}' : ']';
    int depth = 0;
    int in_str = 0;
    while (p < end) {
      char c = *p++;
      if (in_str) {
        if (c == '\\' && p < end) { p++; continue; }
        if (c == '"') in_str = 0;
      } else {
        if (c == '"') in_str = 1;
        else if (c == open_ch) depth++;
        else if (c == close_ch) {
          depth--;
          if (depth == 0) break;
        }
      }
    }
    if (depth != 0) return 0;
  } else if (first == '"') {
    /* Treat as a string: scan to matching unescaped quote. */
    p++;
    while (p < end) {
      char c = *p++;
      if (c == '\\' && p < end) { p++; continue; }
      if (c == '"') break;
    }
  } else {
    /* Number / true / false / null: read until comma / close / ws. */
    while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\n' && *p != '\r') {
      p++;
    }
  }

  size_t vlen = (size_t)(p - vstart);
  if (*arena_used + vlen + 1 > arena_cap) return 0;
  memcpy(arena + *arena_used, vstart, vlen);
  *out_off = *arena_used;
  *out_len = vlen;
  *arena_used += vlen;
  arena[*arena_used] = '\0';
  (*arena_used)++;

  *pp = p;
  return 1;
}

/* Parse one JSONL line into a pre-zeroed xAgentSessionMsg whose
 * string pointers land inside @p arena. Returns 1 on success, 0
 * on any parse error (caller drops the entry). */
static int parse_line_(const char *line, size_t line_len, char *arena,
                       size_t arena_cap, size_t *arena_used,
                       xAgentSessionMsg *out) {
  const char *p   = line;
  const char *end = line + line_len;

  p = skip_ws_(p, end);
  if (p >= end || *p != '{') return 0;
  p++;

  /* Defaults so partially-populated lines still have a sane shape. */
  out->role = xAgentRole_System;
  out->kind = xAgentSessionEntryKind_Text;

  int first = 1;
  while (1) {
    p = skip_ws_(p, end);
    if (p >= end) return 0;
    if (*p == '}') { p++; break; }
    if (!first) {
      if (*p != ',') return 0;
      p++;
      p = skip_ws_(p, end);
    }
    first = 0;

    /* key */
    size_t key_off = 0, key_len = 0;
    if (!parse_string_into_arena_(&p, end, arena, arena_cap, arena_used,
                                  &key_off, &key_len))
      return 0;
    const char *key = arena + key_off;

    p = skip_ws_(p, end);
    if (p >= end || *p != ':') return 0;
    p++;
    p = skip_ws_(p, end);
    if (p >= end) return 0;

    /* Dispatch on key. We rewind *arena_used after reading the key
     * only for keys we don't care about — in the happy path the key
     * bytes stay in the arena as scratch (not referenced by the
     * output) and just consume a few bytes per line. */

    if (*p == '"') {
      /* Value is a string. */
      size_t v_off = 0, v_len = 0;
      if (!parse_string_into_arena_(&p, end, arena, arena_cap, arena_used,
                                    &v_off, &v_len))
        return 0;
      const char *v = arena + v_off;

      if (strcmp(key, "role") == 0) {
        if (strncmp(v, "system", v_len) == 0 && v_len == 6)
          out->role = xAgentRole_System;
        else if (strncmp(v, "user", v_len) == 0 && v_len == 4)
          out->role = xAgentRole_User;
        else if (strncmp(v, "assistant", v_len) == 0 && v_len == 9)
          out->role = xAgentRole_Assistant;
        else if (strncmp(v, "tool", v_len) == 0 && v_len == 4)
          out->role = xAgentRole_Tool;
        else if (strncmp(v, "summary", v_len) == 0 && v_len == 7)
          out->role = xAgentRole_Summary;
      } else if (strcmp(key, "kind") == 0) {
        if (strncmp(v, "text", v_len) == 0 && v_len == 4)
          out->kind = xAgentSessionEntryKind_Text;
        else if (strncmp(v, "thinking", v_len) == 0 && v_len == 8)
          out->kind = xAgentSessionEntryKind_Thinking;
        else if (strncmp(v, "tool_use", v_len) == 0 && v_len == 8)
          out->kind = xAgentSessionEntryKind_ToolUse;
        else if (strncmp(v, "tool_result", v_len) == 0 && v_len == 11)
          out->kind = xAgentSessionEntryKind_ToolResult;
      } else if (strcmp(key, "text") == 0) {
        out->text     = v;
        out->text_len = v_len;
      } else if (strcmp(key, "tool_use_id") == 0) {
        out->tool_use_id = v;
      } else if (strcmp(key, "tool_use_name") == 0) {
        out->tool_use_name = v;
      } else if (strcmp(key, "tool_result_id") == 0) {
        out->tool_result_id = v;
      } else if (strcmp(key, "output") == 0) {
        out->tool_result_output     = v;
        out->tool_result_output_len = v_len;
      }
      /* Unknown string keys: value already copied; harmless. */
    } else if (*p == 't' || *p == 'f' || *p == 'n') {
      /* Bool / null. */
      if (match_lit_(&p, end, "true")) {
        if (strcmp(key, "is_error") == 0) out->tool_result_is_error = 1;
      } else if (match_lit_(&p, end, "false")) {
        if (strcmp(key, "is_error") == 0) out->tool_result_is_error = 0;
      } else if (match_lit_(&p, end, "null")) {
        /* ignore */
      } else {
        return 0;
      }
    } else {
      /* Object / array / number: only tool_use_args expects raw JSON;
       * everything else is still parsed (so we advance past it) but
       * the result is only retained for tool_use_args. */
      size_t v_off = 0, v_len = 0;
      if (!parse_raw_value_into_arena_(&p, end, arena, arena_cap, arena_used,
                                       &v_off, &v_len))
        return 0;
      if (strcmp(key, "tool_use_args") == 0) {
        out->tool_use_args = arena + v_off;
      }
    }
  }

  return 1;
}

/* ── Vtable implementations ────────────────────────────────────────── */

static xErrno jsonl_append_(xAgentMemory store, const xAgentMemoryQuery *query,
                            xAgentMemoryAppendReason reason,
                            const xAgentSessionMsg *msgs, size_t n_msgs) {
  (void)reason; /* Append shape is identical for every reason today */
  struct memory_jsonl_ *b = (struct memory_jsonl_ *)store;

  if (!query->session_id) return xErrno_InvalidArg;

  char *path = build_path_(b, query->session_id);
  if (!path) return xErrno_NoMemory;

  if (mk_parent_dirs_(path) != 0) {
    free(path);
    return xErrno_SysError;
  }

  FILE *fp = fopen(path, "a");
  if (!fp) {
    free(path);
    return xErrno_SysError;
  }

  for (size_t i = 0; i < n_msgs; i++) write_msg_line_(fp, &msgs[i]);

  fclose(fp);
  free(path);
  return xErrno_Ok;
}

/* Internal per-call state anchored on xAgentMemoryHits::cookie.
 * Owns the arena strings referenced by every entries[i].* and the
 * entries[] array itself. */
struct jsonl_hit_cookie_ {
  xAgentSessionMsg *entries;
  char             *arena;
};

static void jsonl_release_(xAgentMemory store, xAgentMemoryHits *hits) {
  (void)store;
  if (!hits || !hits->cookie) return;
  struct jsonl_hit_cookie_ *c = (struct jsonl_hit_cookie_ *)hits->cookie;
  free(c->entries);
  free(c->arena);
  free(c);
  /* memory.c zeroes hits after we return. */
}

/* Read entire file into @p out (malloc'd). Returns 0 on success, -1
 * on any error. *size receives the byte count. */
static int slurp_(const char *path, char **out, size_t *size) {
  FILE *fp = fopen(path, "r");
  if (!fp) return -1;
  if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
  long sz = ftell(fp);
  if (sz < 0) { fclose(fp); return -1; }
  if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { fclose(fp); return -1; }
  size_t got = fread(buf, 1, (size_t)sz, fp);
  fclose(fp);
  buf[got] = '\0';
  *out  = buf;
  *size = got;
  return 0;
}

static xErrno jsonl_retrieve_(xAgentMemory store,
                              const xAgentMemoryQuery *query,
                              xAgentMemoryHits *out) {
  struct memory_jsonl_ *b = (struct memory_jsonl_ *)store;
  if (!query->session_id) return xErrno_InvalidArg;

  /* Per-turn retrieval short-circuit.
   *
   * xAgentMemoryQuery uses recent_turn as the "this is a per-turn
   * retrieval triggered by the given user message" signal. Smarter
   * backends (vector search, reranking, remote memory service) are
   * expected to compute similarity against recent_turn and return a
   * context-relevant slice.
   *
   * The built-in JSONL backend has no index and no embedding — the
   * only thing it can do is return the tail, which the session layer
   * already has via the Create-time prime path. Returning the same
   * tail again on every input would (a) double-inject it into every
   * prompt and (b) bill the caller twice for the same tokens. So we
   * treat per-turn calls as a no-op here and leave it to a future
   * backend that actually knows what to do with recent_turn.
   *
   * The Create-time prime path always calls with recent_turn == NULL,
   * so it's unaffected.
   */
  if (query->recent_turn) {
    /* Leave *out as the caller zeroed it: 0 entries, NULL cookie. */
    return xErrno_Ok;
  }

  /* Resolve the per-call cap: caller hint wins; otherwise backend
   * default. */
  size_t cap = query->max_entries ? query->max_entries : b->default_max_entries;
  if (cap == 0) cap = XAGENT_MEMORY_JSONL_DEFAULT_MAX_ENTRIES;

  char *path = build_path_(b, query->session_id);
  if (!path) return xErrno_NoMemory;

  char  *content = NULL;
  size_t csize   = 0;
  int    ok      = slurp_(path, &content, &csize);
  free(path);
  if (ok != 0) {
    /* File missing is not an error — just empty result. */
    return xErrno_Ok;
  }

  /* Walk the buffer, record each line's [start, end) as a span.
   * We keep ALL line spans so we can pick the last @p cap valid
   * ones after parsing. */
  struct line_span_ { size_t off, len; };
  struct line_span_ *spans = NULL;
  size_t n_spans = 0, cap_spans = 0;

  size_t i = 0;
  while (i < csize) {
    size_t start = i;
    while (i < csize && content[i] != '\n') i++;
    size_t end = i;
    if (i < csize) i++; /* skip the '\n' */

    /* Trim trailing \r. */
    while (end > start && content[end - 1] == '\r') end--;
    if (end == start) continue; /* blank line */

    if (n_spans == cap_spans) {
      size_t new_cap = cap_spans ? cap_spans * 2 : 16;
      struct line_span_ *nn = (struct line_span_ *)realloc(
        spans, new_cap * sizeof(*spans));
      if (!nn) {
        free(spans);
        free(content);
        return xErrno_NoMemory;
      }
      spans     = nn;
      cap_spans = new_cap;
    }
    spans[n_spans].off = start;
    spans[n_spans].len = end - start;
    n_spans++;
  }

  /* Determine the parse window.
   *
   * First, locate the last summary line in the file. A summary
   * absorbs all prior entries, so we never need to load anything
   * before it. We do a lightweight substring search on the raw
   * buffer rather than a full parse — just enough to find lines
   * containing "role":"summary". */
  size_t summary_span = (size_t)-1; /* index into spans[] */
  for (ssize_t k = (ssize_t)n_spans - 1; k >= 0; k--) {
    const char *line = content + spans[k].off;
    size_t      llen = spans[k].len;
    /* Quick scan for the summary role marker. We search for the
     * literal key rather than doing a full JSON parse. The
     * false-positive rate is effectively zero because this
     * key is only written by write_msg_line_. */
    int found = 0;
    for (size_t j = 0; !found && j + 15 <= llen; j++) {
      if (memcmp(line + j, "\"role\":\"summary\"", 16) == 0) {
        summary_span = (size_t)k;
        found = 1;
      }
    }
    if (found) break;
  }

  /* take_from is the greater of the summary-boundary and the
   * tail-cap boundary. This ensures we never load entries that
   * precede the last summary, even if the cap window would have
   * included them. */
  size_t take_from = (n_spans > cap) ? (n_spans - cap) : 0;
  if (summary_span != (size_t)-1 && summary_span > take_from) {
    take_from = summary_span;
  }
  size_t window = n_spans - take_from;
  if (window == 0) {
    free(spans);
    free(content);
    return xErrno_Ok;
  }

  /* Allocate the arena. A generous upper bound is the total size
   * of the windowed lines (escape expansions never grow a line).
   * +1 per line to hold a NUL terminator per string we might stash
   * plus some slack for multiple strings per line. We multiply by
   * 2 for safety. */
  size_t arena_cap = 0;
  for (size_t k = 0; k < window; k++) arena_cap += spans[take_from + k].len;
  arena_cap = arena_cap * 2 + 32;

  char *arena = (char *)malloc(arena_cap);
  xAgentSessionMsg *entries =
    (xAgentSessionMsg *)calloc(window, sizeof(*entries));
  if (!arena || !entries) {
    free(arena);
    free(entries);
    free(spans);
    free(content);
    return xErrno_NoMemory;
  }

  size_t arena_used = 0;
  size_t n_out      = 0;
  for (size_t k = 0; k < window; k++) {
    const char *line = content + spans[take_from + k].off;
    size_t      llen = spans[take_from + k].len;
    xAgentSessionMsg tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t arena_snapshot = arena_used;
    if (parse_line_(line, llen, arena, arena_cap, &arena_used, &tmp)) {
      entries[n_out++] = tmp;
    } else {
      /* Roll the arena back so the bad line doesn't consume space. */
      arena_used = arena_snapshot;
    }
  }

  free(spans);
  free(content);

  if (n_out == 0) {
    free(entries);
    free(arena);
    return xErrno_Ok;
  }

  /* Safety net: if the span-level scan missed a summary role
   * (e.g., encoding edge case), the parse-level scan catches it.
   * Most of the time this block is a no-op because take_from was
   * already adjusted to start at the last summary. */
  {
    ssize_t summary_idx = -1;
    for (ssize_t k = (ssize_t)n_out - 1; k >= 0; k--) {
      if (entries[k].role == xAgentRole_Summary) {
        summary_idx = k;
        break;
      }
    }
    if (summary_idx > 0) {
      /* Shift entries down so the summary becomes entries[0].
       * The arena pointers are still valid — they point into the
       * same arena regardless of array position. We don't reclaim
       * the arena bytes for dropped entries, but the waste is
       * bounded by the pre-summary portion of the tail window. */
      size_t drop = (size_t)summary_idx;
      size_t keep = n_out - drop;
      memmove(entries, entries + drop, keep * sizeof(*entries));
      n_out = keep;
    }
  }

  struct jsonl_hit_cookie_ *cookie =
    (struct jsonl_hit_cookie_ *)calloc(1, sizeof(*cookie));
  if (!cookie) {
    free(entries);
    free(arena);
    return xErrno_NoMemory;
  }
  cookie->entries = entries;
  cookie->arena   = arena;

  out->entries   = entries;
  out->n_entries = n_out;
  out->cookie    = cookie;
  return xErrno_Ok;
}

static void jsonl_destroy_(xAgentMemory store) {
  struct memory_jsonl_ *b = (struct memory_jsonl_ *)store;
  free(b->root_dir);
  free(b);
}

/* No on_session_open / on_session_close — the JSONL backend is
 * stateless per session and builds the path lazily. */

static const xAgentMemoryVTable kJsonlVT = {
  /* append           */ jsonl_append_,
  /* retrieve         */ jsonl_retrieve_,
  /* release          */ jsonl_release_,
  /* on_session_open  */ NULL,
  /* on_session_close */ NULL,
  /* destroy          */ jsonl_destroy_,
};

/* ── Factory ───────────────────────────────────────────────────────── */

xAgentMemory xAgentMemoryJsonlCreate(const xAgentMemoryJsonlConf *conf) {
  if (!conf || !conf->root_dir || !*conf->root_dir) return NULL;

  struct memory_jsonl_ *b =
    (struct memory_jsonl_ *)calloc(1, sizeof(*b));
  if (!b) return NULL;

  b->base.vt   = &kJsonlVT;
  b->root_dir  = strdup(conf->root_dir);
  b->default_max_entries = conf->default_max_entries
                             ? conf->default_max_entries
                             : XAGENT_MEMORY_JSONL_DEFAULT_MAX_ENTRIES;
  if (!b->root_dir) {
    free(b);
    return NULL;
  }

  return (xAgentMemory)b;
}
