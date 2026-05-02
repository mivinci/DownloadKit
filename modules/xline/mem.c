/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mem.c - Custom allocator adapter
 */
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "str.h" // ic_strlen, ic_memcpy

/* ── Heap allocation. xline forks isocline without the custom-allocator
 * indirection. The `alloc_t*` parameter is kept in the signature for source
 * compatibility with the ~50 internal call sites (and because the
 * `env->mem`/`bb->mem`/... fields still thread through them), but the pointer
 * itself is unused: we always call the stdlib directly ── */

ic_private void *mem_malloc(alloc_t *mem, ssize_t sz) {
  ic_unused(mem);
  return malloc(to_size_t(sz));
}

ic_private void *mem_zalloc(alloc_t *mem, ssize_t sz) {
  void *p = mem_malloc(mem, sz);
  if (p != NULL) memset(p, 0, to_size_t(sz));
  return p;
}

ic_private void *mem_realloc(alloc_t *mem, void *p, ssize_t newsz) {
  ic_unused(mem);
  return realloc(p, to_size_t(newsz));
}

ic_private void mem_free(alloc_t *mem, const void *p) {
  ic_unused(mem);
  free((void *)p);
}

ic_private char *mem_strdup(alloc_t *mem, const char *s) {
  if (s == NULL) return NULL;
  ssize_t n = ic_strlen(s);
  char   *p = mem_malloc_tp_n(mem, char, n + 1);
  if (p == NULL) return NULL;
  ic_memcpy(p, s, n + 1);
  return p;
}

ic_private char *mem_strndup(alloc_t *mem, const char *s, ssize_t n) {
  if (s == NULL || n < 0) return NULL;
  char *p = mem_malloc_tp_n(mem, char, n + 1);
  if (p == NULL) return NULL;
  ssize_t i;
  for (i = 0; i < n && s[i] != 0; i++) {
    p[i] = s[i];
  }
  assert(i <= n);
  p[i] = 0;
  return p;
}
