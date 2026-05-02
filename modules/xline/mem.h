/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef IC_MEM_H
#define IC_MEM_H

//-------------------------------------------------------------
// Heap allocation.
//
// xline always uses the stdlib allocator; the alloc_t struct is
// retained as an opaque tag so ~50 internal call sites that pass
// `env->mem`, `bb->mem`, etc. compile unchanged. The function-pointer
// indirection of upstream isocline has been removed — mem_* helpers
// ignore the alloc_t* argument entirely.
//-------------------------------------------------------------

#include "platform.h"

typedef struct alloc_s {
  int _unused; // placeholder; C forbids empty structs
} alloc_t;

ic_private void *mem_malloc(alloc_t *mem, ssize_t sz);
ic_private void *mem_zalloc(alloc_t *mem, ssize_t sz);
ic_private void *mem_realloc(alloc_t *mem, void *p, ssize_t newsz);
ic_private void  mem_free(alloc_t *mem, const void *p);
ic_private char *mem_strdup(alloc_t *mem, const char *s);
ic_private char *mem_strndup(alloc_t *mem, const char *s, ssize_t n);

#define mem_zalloc_tp(mem, tp)      (tp *)mem_zalloc(mem, ssizeof(tp))
#define mem_malloc_tp_n(mem, tp, n) (tp *)mem_malloc(mem, (n) * ssizeof(tp))
#define mem_zalloc_tp_n(mem, tp, n) (tp *)mem_zalloc(mem, (n) * ssizeof(tp))
#define mem_realloc_tp(mem, tp, p, n) \
  (tp *)mem_realloc(mem, p, (n) * ssizeof(tp))

#endif // IC_MEM_H
