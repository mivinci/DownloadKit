/*
 * Copyright 2026 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * flag.c - Command-line flag parser implementation
 */

#include <xbase/flag.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ───────────────────── Internal types ───────────────────── */

enum xFlagKind_ {
  xFlagKind_String = 0,
  xFlagKind_Bool,
  xFlagKind_Int,
  xFlagKind_I64,
  xFlagKind_U64,
  xFlagKind_Double,
  xFlagKind_Choice,
  xFlagKind_Counter,
};

struct xFlagEntry_ {
  struct xFlagEntry_ *next;

  char               *name;   /**< Long name (may be NULL)      */
  char                shortc; /**< Short name, 0 if unused      */
  char               *meta;   /**< Placeholder (may be NULL)    */
  char               *help;   /**< One-line description         */
  char               *def;    /**< Default value as string      */
  int                 attrs;  /**< xFlagAttr bitmask            */
  enum xFlagKind_     kind;
  int                 seen;   /**< Occurrence count             */

  /* Caller-owned storage pointer. Discriminated by @c kind.    */
  union {
    const char **s;
    bool        *b;
    int         *i;
    int64_t     *i64;
    uint64_t    *u64;
    double      *d;
    int         *cnt;
  } out;

  /* Choice metadata (xFlagKind_Choice only). Not owned.        */
  const char *const *choices;

  /* xFlagAttr_Multi storage (strings only). Owned by set.      */
  const char **multi_values;
  size_t       multi_count;
  size_t       multi_cap;
};

struct xFlagPositional_ {
  struct xFlagPositional_ *next;

  char        *name;
  char        *help;
  int          attrs;
  bool         is_tail;

  /* Single positional storage                                  */
  const char **out_single;

  /* Tail storage (xFlagAddPositionalTail)                      */
  const char ***out_tail;
  size_t        *out_tail_count;
  const char   **tail_buf; /**< Owned NUL-terminated array     */
};

struct xFlagSet_ {
  char *prog;
  char *summary;
  char *epilog;
  char *version;

  struct xFlagEntry_      *flags;     /**< Head of registration order   */
  struct xFlagPositional_ *positionals;
  struct xFlagPositional_ *tail;      /**< Optional single tail entry   */

  size_t n_flags;
  size_t n_positionals;
};

/* ───────────────────── Small utilities ───────────────────── */

static char *xFlagStrdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char  *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

/* printf-style allocator; never returns NULL unless OOM.       */
static char *xFlagAsprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    va_end(ap2);
    return NULL;
  }
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  return buf;
}

static void xFlagSetErr(char **err_out, const char *fmt, ...) {
  if (!err_out) return;
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    va_end(ap2);
    return;
  }
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  *err_out = buf;
}

/* ───────────────────── Lookup helpers ───────────────────── */

static struct xFlagEntry_ *xFlagFindByLong(struct xFlagSet_ *set,
                                           const char *name, size_t len) {
  for (struct xFlagEntry_ *e = set->flags; e; e = e->next) {
    if (!e->name) continue;
    if (strncmp(e->name, name, len) == 0 && e->name[len] == '\0') {
      return e;
    }
  }
  return NULL;
}

static struct xFlagEntry_ *xFlagFindByShort(struct xFlagSet_ *set, char c) {
  for (struct xFlagEntry_ *e = set->flags; e; e = e->next) {
    if (e->shortc && e->shortc == c) return e;
  }
  return NULL;
}

static bool xFlagNeedsValue(const struct xFlagEntry_ *e) {
  return e->kind != xFlagKind_Bool && e->kind != xFlagKind_Counter;
}

/* ───────────────────── Type conversion ───────────────────── */

static bool xFlagParseI64(const char *s, int64_t *out) {
  if (!s || !*s) return false;
  errno = 0;
  char    *end = NULL;
  long long v  = strtoll(s, &end, 0); /* auto base: 0x/0b/0     */
  /* strtoll does not handle 0b prefix; do it manually.         */
  if ((s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) ||
      (s[0] == '-' && s[1] == '0' && (s[2] == 'b' || s[2] == 'B'))) {
    int sign = 1;
    const char *p = s;
    if (*p == '-') { sign = -1; ++p; }
    p += 2; /* skip 0b */
    if (!*p) return false;
    unsigned long long acc = 0;
    for (; *p; ++p) {
      if (*p != '0' && *p != '1') return false;
      acc = (acc << 1) | (unsigned)(*p - '0');
    }
    *out = sign < 0 ? -(int64_t)acc : (int64_t)acc;
    return true;
  }
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = (int64_t)v;
  return true;
}

static bool xFlagParseU64(const char *s, uint64_t *out) {
  if (!s || !*s || *s == '-') return false;
  errno = 0;
  char              *end = NULL;
  unsigned long long v   = strtoull(s, &end, 0);
  if ((s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))) {
    const char *p = s + 2;
    if (!*p) return false;
    unsigned long long acc = 0;
    for (; *p; ++p) {
      if (*p != '0' && *p != '1') return false;
      acc = (acc << 1) | (unsigned)(*p - '0');
    }
    *out = acc;
    return true;
  }
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = (uint64_t)v;
  return true;
}

static bool xFlagParseDouble(const char *s, double *out) {
  if (!s || !*s) return false;
  errno = 0;
  char  *end = NULL;
  double v   = strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0') return false;
  *out = v;
  return true;
}

/* Apply a value string to @p e, writing to caller storage.     */
static xErrno xFlagApplyValue(struct xFlagEntry_ *e, const char *val,
                              char **err_out) {
  switch (e->kind) {
  case xFlagKind_String:
    *e->out.s = val;
    if (e->attrs & xFlagAttr_Multi) {
      if (e->multi_count + 1 >= e->multi_cap) {
        size_t       cap = e->multi_cap ? e->multi_cap * 2 : 4;
        const char **p =
          (const char **)realloc(e->multi_values, cap * sizeof(*p));
        if (!p) return xErrno_NoMemory;
        e->multi_values = p;
        e->multi_cap    = cap;
      }
      e->multi_values[e->multi_count++] = val;
      e->multi_values[e->multi_count]   = NULL;
    }
    return xErrno_Ok;

  case xFlagKind_Int: {
    int64_t v = 0;
    if (!xFlagParseI64(val, &v) || v < INT_MIN || v > INT_MAX) {
      xFlagSetErr(err_out, "invalid integer for --%s: '%s'",
                  e->name ? e->name : "?", val);
      return xErrno_InvalidArg;
    }
    *e->out.i = (int)v;
    return xErrno_Ok;
  }
  case xFlagKind_I64: {
    int64_t v = 0;
    if (!xFlagParseI64(val, &v)) {
      xFlagSetErr(err_out, "invalid integer for --%s: '%s'",
                  e->name ? e->name : "?", val);
      return xErrno_InvalidArg;
    }
    *e->out.i64 = v;
    return xErrno_Ok;
  }
  case xFlagKind_U64: {
    uint64_t v = 0;
    if (!xFlagParseU64(val, &v)) {
      xFlagSetErr(err_out, "invalid unsigned for --%s: '%s'",
                  e->name ? e->name : "?", val);
      return xErrno_InvalidArg;
    }
    *e->out.u64 = v;
    return xErrno_Ok;
  }
  case xFlagKind_Double: {
    double v = 0.0;
    if (!xFlagParseDouble(val, &v)) {
      xFlagSetErr(err_out, "invalid number for --%s: '%s'",
                  e->name ? e->name : "?", val);
      return xErrno_InvalidArg;
    }
    *e->out.d = v;
    return xErrno_Ok;
  }
  case xFlagKind_Choice: {
    for (const char *const *c = e->choices; c && *c; ++c) {
      if (strcmp(*c, val) == 0) {
        *e->out.s = *c;
        return xErrno_Ok;
      }
    }
    /* Build a human-friendly list of choices.                  */
    size_t sz = 1;
    for (const char *const *c = e->choices; c && *c; ++c)
      sz += strlen(*c) + 2;
    char *list = (char *)malloc(sz);
    if (!list) return xErrno_NoMemory;
    list[0] = '\0';
    for (const char *const *c = e->choices; c && *c; ++c) {
      if (list[0]) strcat(list, ", ");
      strcat(list, *c);
    }
    xFlagSetErr(err_out, "invalid choice for --%s: '%s' (allowed: %s)",
                e->name ? e->name : "?", val, list);
    free(list);
    return xErrno_InvalidArg;
  }

  case xFlagKind_Bool:
  case xFlagKind_Counter:
    /* Not reachable: no-arg flags go through xFlagApplyFlag.   */
    return xErrno_InvalidArg;
  }
  return xErrno_Unknown;
}

/* ───────────────────── Registration ───────────────────── */

static xErrno xFlagSetAddEntry(struct xFlagSet_ *set, struct xFlagEntry_ *e,
                               const char *name, char shortc) {
  if (!set || !e) return xErrno_InvalidArg;

  /* Duplicate detection.                                       */
  if (name) {
    for (struct xFlagEntry_ *x = set->flags; x; x = x->next) {
      if (x->name && strcmp(x->name, name) == 0) {
        return xErrno_AlreadyExists;
      }
    }
  }
  if (shortc) {
    for (struct xFlagEntry_ *x = set->flags; x; x = x->next) {
      if (x->shortc == shortc) return xErrno_AlreadyExists;
    }
  }

  /* Append to list (preserve registration order).              */
  if (!set->flags) {
    set->flags = e;
  } else {
    struct xFlagEntry_ *x = set->flags;
    while (x->next) x = x->next;
    x->next = e;
  }
  set->n_flags++;
  return xErrno_Ok;
}

static struct xFlagEntry_ *xFlagEntryNew(const char *name, char shortc,
                                         const char *meta, const char *help,
                                         int attrs, enum xFlagKind_ kind) {
  struct xFlagEntry_ *e = (struct xFlagEntry_ *)calloc(1, sizeof(*e));
  if (!e) return NULL;
  e->name   = xFlagStrdup(name);
  e->shortc = shortc;
  e->meta   = xFlagStrdup(meta);
  e->help   = xFlagStrdup(help);
  e->attrs  = attrs;
  e->kind   = kind;
  if ((name && !e->name) || (meta && !e->meta) || (help && !e->help)) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e);
    return NULL;
  }
  return e;
}

/* ───────────────────── Public API: lifecycle ───────────────────── */

XCAPI(xFlagSet) xFlagSetCreate(const char *prog, const char *summary) {
  if (!prog) return NULL;
  struct xFlagSet_ *s = (struct xFlagSet_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->prog    = xFlagStrdup(prog);
  s->summary = xFlagStrdup(summary);
  if (!s->prog || (summary && !s->summary)) {
    free(s->prog);
    free(s->summary);
    free(s);
    return NULL;
  }
  return s;
}

XCAPI(void) xFlagSetDestroy(xFlagSet set_) {
  if (!set_) return;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;

  struct xFlagEntry_ *e = set->flags;
  while (e) {
    struct xFlagEntry_ *n = e->next;
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e->multi_values);
    free(e);
    e = n;
  }

  struct xFlagPositional_ *p = set->positionals;
  while (p) {
    struct xFlagPositional_ *n = p->next;
    free(p->name);
    free(p->help);
    free(p->tail_buf);
    free(p);
    p = n;
  }
  if (set->tail) {
    free(set->tail->name);
    free(set->tail->help);
    free(set->tail->tail_buf);
    free(set->tail);
  }

  free(set->prog);
  free(set->summary);
  free(set->epilog);
  free(set->version);
  free(set);
}

XCAPI(void) xFlagSetEpilog(xFlagSet set_, const char *text) {
  if (!set_) return;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  free(set->epilog);
  set->epilog = xFlagStrdup(text);
}

XCAPI(void) xFlagSetVersion(xFlagSet set_, const char *version) {
  if (!set_) return;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  free(set->version);
  set->version = xFlagStrdup(version);
}

/* ───────────────────── Public API: add scalars ───────────────────── */

XCAPI(xErrno) xFlagAddString(xFlagSet set_, const char *name, char shortc,
                             const char *meta, const char *help,
                             const char **storage, const char *def,
                             int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, meta, help, attrs, xFlagKind_String);
  if (!e) return xErrno_NoMemory;
  e->out.s = storage;
  e->def   = xFlagStrdup(def);
  if (def && !e->def) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e);
    return xErrno_NoMemory;
  }
  *storage = def; /* initialise to default */
  xErrno rc = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddBool(xFlagSet set_, const char *name, char shortc,
                           const char *help, bool *storage, int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, NULL, help, attrs, xFlagKind_Bool);
  if (!e) return xErrno_NoMemory;
  e->out.b = storage;
  *storage = false;
  xErrno rc = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->help);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddInt(xFlagSet set_, const char *name, char shortc,
                          const char *meta, const char *help,
                          int *storage, int def, int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, meta, help, attrs, xFlagKind_Int);
  if (!e) return xErrno_NoMemory;
  e->out.i = storage;
  e->def   = xFlagAsprintf("%d", def);
  *storage = def;
  xErrno rc = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddI64(xFlagSet set_, const char *name, char shortc,
                          const char *meta, const char *help,
                          int64_t *storage, int64_t def, int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, meta, help, attrs, xFlagKind_I64);
  if (!e) return xErrno_NoMemory;
  e->out.i64 = storage;
  e->def     = xFlagAsprintf("%" PRId64, def);
  *storage   = def;
  xErrno rc  = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddU64(xFlagSet set_, const char *name, char shortc,
                          const char *meta, const char *help,
                          uint64_t *storage, uint64_t def, int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, meta, help, attrs, xFlagKind_U64);
  if (!e) return xErrno_NoMemory;
  e->out.u64 = storage;
  e->def     = xFlagAsprintf("%" PRIu64, def);
  *storage   = def;
  xErrno rc  = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddDouble(xFlagSet set_, const char *name, char shortc,
                             const char *meta, const char *help,
                             double *storage, double def, int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, meta, help, attrs, xFlagKind_Double);
  if (!e) return xErrno_NoMemory;
  e->out.d = storage;
  e->def   = xFlagAsprintf("%g", def);
  *storage = def;
  xErrno rc = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddChoice(xFlagSet set_, const char *name, char shortc,
                             const char *meta, const char *help,
                             const char *const *choices,
                             const char **storage, const char *def,
                             int attrs) {
  if (!set_ || !storage || !choices || (!name && !shortc))
    return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, meta, help, attrs, xFlagKind_Choice);
  if (!e) return xErrno_NoMemory;
  e->out.s   = storage;
  e->choices = choices;
  e->def     = xFlagStrdup(def);
  *storage   = def;
  xErrno rc  = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->meta);
    free(e->help);
    free(e->def);
    free(e);
  }
  return rc;
}

XCAPI(xErrno) xFlagAddCounter(xFlagSet set_, const char *name, char shortc,
                              const char *help, int *storage, int attrs) {
  if (!set_ || !storage || (!name && !shortc)) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  struct xFlagEntry_ *e =
    xFlagEntryNew(name, shortc, NULL, help, attrs, xFlagKind_Counter);
  if (!e) return xErrno_NoMemory;
  e->out.cnt = storage;
  *storage   = 0;
  xErrno rc  = xFlagSetAddEntry(set, e, name, shortc);
  if (rc != xErrno_Ok) {
    free(e->name);
    free(e->help);
    free(e);
  }
  return rc;
}

/* ───────────────────── Public API: positionals ───────────────────── */

XCAPI(xErrno) xFlagAddPositional(xFlagSet set_, const char *name,
                                 const char *help, const char **storage,
                                 int attrs) {
  if (!set_ || !storage || !name) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  if (set->tail) {
    /* Positionals after a tail are not allowed.                */
    return xErrno_InvalidArg;
  }
  struct xFlagPositional_ *p =
    (struct xFlagPositional_ *)calloc(1, sizeof(*p));
  if (!p) return xErrno_NoMemory;
  p->name       = xFlagStrdup(name);
  p->help       = xFlagStrdup(help);
  p->attrs      = attrs;
  p->out_single = storage;
  *storage      = NULL;

  if (!set->positionals) {
    set->positionals = p;
  } else {
    struct xFlagPositional_ *x = set->positionals;
    while (x->next) x = x->next;
    x->next = p;
  }
  set->n_positionals++;
  return xErrno_Ok;
}

XCAPI(xErrno) xFlagAddPositionalTail(xFlagSet set_, const char *name,
                                     const char *help,
                                     const char ***storage, size_t *count,
                                     int attrs) {
  if (!set_ || !storage || !name) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  if (set->tail) return xErrno_AlreadyExists;
  struct xFlagPositional_ *p =
    (struct xFlagPositional_ *)calloc(1, sizeof(*p));
  if (!p) return xErrno_NoMemory;
  p->name           = xFlagStrdup(name);
  p->help           = xFlagStrdup(help);
  p->attrs          = attrs;
  p->is_tail        = true;
  p->out_tail       = storage;
  p->out_tail_count = count;
  *storage          = NULL;
  if (count) *count = 0;
  set->tail = p;
  return xErrno_Ok;
}

/* ───────────────────── Parse engine ───────────────────── */

/* Apply an occurrence of a flag entry; consumes @p value if needed.
 * Returns xErrno_Ok or sets @c *err_out on failure.             */
static xErrno xFlagApplyFlag(struct xFlagEntry_ *e, const char *value,
                             char **err_out) {
  e->seen++;
  switch (e->kind) {
  case xFlagKind_Bool:
    *e->out.b = true;
    return xErrno_Ok;
  case xFlagKind_Counter:
    (*e->out.cnt)++;
    return xErrno_Ok;
  default:
    if (!value) {
      xFlagSetErr(err_out, "flag --%s requires a value",
                  e->name ? e->name : "?");
      return xErrno_InvalidArg;
    }
    return xFlagApplyValue(e, value, err_out);
  }
}

XCAPI(xErrno) xFlagParse(xFlagSet set_, int argc, char *const argv[],
                         char **err_out) {
  if (err_out) *err_out = NULL;
  if (!set_ || argc < 1 || !argv) return xErrno_InvalidArg;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;

  int i = 1;
  /* Collected positional values (indexes into argv).            */
  int *pos_idx = NULL;
  int  pos_cnt = 0;
  int  pos_cap = 0;

  while (i < argc) {
    const char *a = argv[i];
    if (!a) break;

    /* End-of-options sentinel.                                  */
    if (strcmp(a, "--") == 0) {
      ++i;
      while (i < argc) {
        if (pos_cnt >= pos_cap) {
          int   ncap = pos_cap ? pos_cap * 2 : 8;
          int  *p    = (int *)realloc(pos_idx, (size_t)ncap * sizeof(int));
          if (!p) { free(pos_idx); return xErrno_NoMemory; }
          pos_idx = p;
          pos_cap = ncap;
        }
        pos_idx[pos_cnt++] = i++;
      }
      break;
    }

    /* "-" alone is a positional (stdin idiom).                  */
    if (a[0] != '-' || a[1] == '\0') {
      if (pos_cnt >= pos_cap) {
        int   ncap = pos_cap ? pos_cap * 2 : 8;
        int  *p    = (int *)realloc(pos_idx, (size_t)ncap * sizeof(int));
        if (!p) { free(pos_idx); return xErrno_NoMemory; }
        pos_idx = p;
        pos_cap = ncap;
      }
      pos_idx[pos_cnt++] = i++;
      continue;
    }

    if (a[1] == '-') {
      /* Long option: --name or --name=value                     */
      const char *name = a + 2;
      const char *eq   = strchr(name, '=');
      size_t      nlen = eq ? (size_t)(eq - name) : strlen(name);

      /* Built-ins: --help / --version                           */
      if (nlen == 4 && strncmp(name, "help", 4) == 0) {
        xFlagPrintHelp(set, stdout);
        free(pos_idx);
        return xErrno_Again;
      }
      if (set->version && nlen == 7 && strncmp(name, "version", 7) == 0) {
        fprintf(stdout, "%s\n", set->version);
        free(pos_idx);
        return xErrno_Again;
      }

      struct xFlagEntry_ *e = xFlagFindByLong(set, name, nlen);
      if (!e) {
        xFlagSetErr(err_out, "unknown option: --%.*s", (int)nlen, name);
        free(pos_idx);
        return xErrno_InvalidArg;
      }

      const char *value = NULL;
      if (xFlagNeedsValue(e)) {
        if (eq) {
          value = eq + 1;
        } else if (i + 1 < argc) {
          value = argv[++i];
        } else {
          xFlagSetErr(err_out, "flag --%s requires a value", e->name);
          free(pos_idx);
          return xErrno_InvalidArg;
        }
      } else if (eq) {
        xFlagSetErr(err_out, "flag --%s takes no value", e->name);
        free(pos_idx);
        return xErrno_InvalidArg;
      }
      xErrno rc = xFlagApplyFlag(e, value, err_out);
      if (rc != xErrno_Ok) { free(pos_idx); return rc; }
      ++i;
      continue;
    }

    /* Short option(s): -a / -abc / -fvalue / -f value           */
    const char *p = a + 1;
    bool        consumed_next = false;
    while (*p) {
      /* Built-ins: -h / -V                                      */
      if (*p == 'h' && !xFlagFindByShort(set, 'h')) {
        xFlagPrintHelp(set, stdout);
        free(pos_idx);
        return xErrno_Again;
      }
      if (*p == 'V' && set->version && !xFlagFindByShort(set, 'V')) {
        fprintf(stdout, "%s\n", set->version);
        free(pos_idx);
        return xErrno_Again;
      }

      struct xFlagEntry_ *e = xFlagFindByShort(set, *p);
      if (!e) {
        xFlagSetErr(err_out, "unknown option: -%c", *p);
        free(pos_idx);
        return xErrno_InvalidArg;
      }

      if (xFlagNeedsValue(e)) {
        const char *value = NULL;
        if (*(p + 1)) {
          value = p + 1; /* glued: -fvalue */
        } else if (i + 1 < argc) {
          value         = argv[++i];
          consumed_next = true;
        } else {
          xFlagSetErr(err_out, "flag -%c requires a value", *p);
          free(pos_idx);
          return xErrno_InvalidArg;
        }
        xErrno rc = xFlagApplyFlag(e, value, err_out);
        if (rc != xErrno_Ok) { free(pos_idx); return rc; }
        break; /* short-with-arg consumes the rest of this token */
      }

      /* No-arg short (bool / counter): keep bundling.           */
      xErrno rc = xFlagApplyFlag(e, NULL, err_out);
      if (rc != xErrno_Ok) { free(pos_idx); return rc; }
      ++p;
    }
    (void)consumed_next;
    ++i;
  }

  /* Required-flag validation.                                   */
  for (struct xFlagEntry_ *e = set->flags; e; e = e->next) {
    if ((e->attrs & xFlagAttr_Required) && e->seen == 0) {
      const char *label = e->name ? e->name : NULL;
      if (label) {
        xFlagSetErr(err_out, "missing required flag: --%s", label);
      } else {
        xFlagSetErr(err_out, "missing required flag: -%c", e->shortc);
      }
      free(pos_idx);
      return xErrno_InvalidArg;
    }
  }

  /* Positional assignment.                                      */
  int p_at = 0;
  struct xFlagPositional_ *pp = set->positionals;
  while (pp) {
    if (p_at < pos_cnt) {
      *pp->out_single = argv[pos_idx[p_at++]];
    } else if (pp->attrs & xFlagAttr_Required) {
      xFlagSetErr(err_out, "missing required argument: %s", pp->name);
      free(pos_idx);
      return xErrno_InvalidArg;
    }
    pp = pp->next;
  }

  if (set->tail) {
    /* Expand remaining positionals into tail array.             */
    int n = pos_cnt - p_at;
    if (n < 0) n = 0;
    const char **buf =
      (const char **)malloc((size_t)(n + 1) * sizeof(*buf));
    if (!buf) { free(pos_idx); return xErrno_NoMemory; }
    for (int k = 0; k < n; ++k) buf[k] = argv[pos_idx[p_at + k]];
    buf[n] = NULL;
    free(set->tail->tail_buf);
    set->tail->tail_buf  = buf;
    *set->tail->out_tail = buf;
    if (set->tail->out_tail_count) *set->tail->out_tail_count = (size_t)n;
    if ((set->tail->attrs & xFlagAttr_Required) && n == 0) {
      xFlagSetErr(err_out, "missing required argument: %s", set->tail->name);
      free(pos_idx);
      return xErrno_InvalidArg;
    }
  } else if (p_at < pos_cnt) {
    xFlagSetErr(err_out, "unexpected argument: %s", argv[pos_idx[p_at]]);
    free(pos_idx);
    return xErrno_InvalidArg;
  }

  free(pos_idx);
  return xErrno_Ok;
}

/* ───────────────────── Output ───────────────────── */

/* "-f, --file FILE" header used in help.                        */
static void xFlagFormatHeader(const struct xFlagEntry_ *e, FILE *fp) {
  bool needs = xFlagNeedsValue(e);
  const char *meta = e->meta ? e->meta : (needs ? "VALUE" : NULL);
  if (e->shortc && e->name) {
    fprintf(fp, "  -%c, --%s", e->shortc, e->name);
  } else if (e->shortc) {
    fprintf(fp, "  -%c", e->shortc);
  } else {
    fprintf(fp, "      --%s", e->name);
  }
  if (needs) fprintf(fp, " %s", meta);
}

XCAPI(void) xFlagPrintUsage(xFlagSet set_, void *fp_) {
  if (!set_ || !fp_) return;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  FILE *fp = (FILE *)fp_;
  fprintf(fp, "USAGE: %s", set->prog);

  bool has_options = false;
  for (struct xFlagEntry_ *e = set->flags; e; e = e->next) {
    if (e->attrs & xFlagAttr_Hidden) continue;
    has_options = true;
    break;
  }
  if (has_options) fprintf(fp, " [OPTIONS]");

  for (struct xFlagPositional_ *p = set->positionals; p; p = p->next) {
    if (p->attrs & xFlagAttr_Required) {
      fprintf(fp, " %s", p->name);
    } else {
      fprintf(fp, " [%s]", p->name);
    }
  }
  if (set->tail) {
    if (set->tail->attrs & xFlagAttr_Required) {
      fprintf(fp, " %s...", set->tail->name);
    } else {
      fprintf(fp, " [%s...]", set->tail->name);
    }
  }
  fprintf(fp, "\n");
}

XCAPI(void) xFlagPrintHelp(xFlagSet set_, void *fp_) {
  if (!set_ || !fp_) return;
  struct xFlagSet_ *set = (struct xFlagSet_ *)set_;
  FILE *fp = (FILE *)fp_;

  xFlagPrintUsage(set, fp);
  if (set->summary) fprintf(fp, "\n%s\n", set->summary);

  /* Positional section.                                         */
  if (set->positionals || set->tail) {
    fprintf(fp, "\nARGUMENTS:\n");
    for (struct xFlagPositional_ *p = set->positionals; p; p = p->next) {
      fprintf(fp, "  %-20s  %s%s\n", p->name, p->help ? p->help : "",
              (p->attrs & xFlagAttr_Required) ? " (required)" : "");
    }
    if (set->tail) {
      fprintf(fp, "  %-20s  %s%s\n", set->tail->name,
              set->tail->help ? set->tail->help : "",
              (set->tail->attrs & xFlagAttr_Required) ? " (required)" : "");
    }
  }

  /* Options section.                                            */
  bool any_visible = false;
  for (struct xFlagEntry_ *e = set->flags; e; e = e->next) {
    if (!(e->attrs & xFlagAttr_Hidden)) { any_visible = true; break; }
  }
  if (any_visible) {
    fprintf(fp, "\nOPTIONS:\n");
    for (struct xFlagEntry_ *e = set->flags; e; e = e->next) {
      if (e->attrs & xFlagAttr_Hidden) continue;
      xFlagFormatHeader(e, fp);
      fprintf(fp, "\n");
      if (e->help && *e->help) fprintf(fp, "        %s", e->help);
      if (e->attrs & xFlagAttr_Required) fprintf(fp, " (required)");
      if (e->def && *e->def && e->kind != xFlagKind_Bool &&
          e->kind != xFlagKind_Counter) {
        fprintf(fp, " [default: %s]", e->def);
      }
      /* List choices when applicable.                           */
      if (e->kind == xFlagKind_Choice && e->choices) {
        fprintf(fp, " (choices:");
        for (const char *const *c = e->choices; *c; ++c) {
          fprintf(fp, " %s", *c);
        }
        fprintf(fp, ")");
      }
      fprintf(fp, "\n");
    }
    /* Always advertise --help and (if set) --version.           */
    fprintf(fp, "  -h, --help\n        show this help and exit\n");
    if (set->version) {
      fprintf(fp, "  -V, --version\n        show version and exit\n");
    }
  }

  if (set->epilog && *set->epilog) fprintf(fp, "\n%s\n", set->epilog);
}
