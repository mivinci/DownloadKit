/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * memory.c - Memory allocation and reference counting
 */

#include <xbase/atomic.h>
#include <xbase/memory.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

XDEF_STRUCT(Header) {
  const char *name; /* for debug */
  size_t      size;
  size_t      refs;
  xVTable    *vtab;
};

void *xAlloc(const char *name, const size_t size, const size_t count,
             xVTable *vtab) {
  Header *hdr;
  void   *ptr;

  hdr = (Header *)malloc(sizeof(Header) + size * count);
  if (!hdr) return NULL;

  hdr->name = name;
  hdr->size = size;
  hdr->refs = 1;
  hdr->vtab = vtab;

  ptr = (void *)(hdr + 1);
  if (vtab->ctor) {
    vtab->ctor(ptr);
  }
  return ptr;
}

void xFree(void *ptr) {
  Header  *hdr;
  xVTable *vtab;

  if (!ptr) return;

  hdr  = (Header *)ptr - 1;
  vtab = hdr->vtab;

  if (vtab && vtab->dtor) {
    vtab->dtor(ptr);
  }
  free(hdr);
}

void xRetain(void *ptr) {
  Header  *hdr;
  xVTable *vtab;

  hdr  = (Header *)ptr - 1;
  vtab = hdr->vtab;

  if (vtab->retain) {
    vtab->retain(ptr);
  }
  xAtomicAdd(&hdr->refs, 1, __ATOMIC_SEQ_CST);
}

void xRelease(void *ptr) {
  Header  *hdr;
  xVTable *vtab;

  hdr  = (Header *)ptr - 1;
  vtab = hdr->vtab;

  if (xAtomicSub(&hdr->refs, 1, __ATOMIC_SEQ_CST) == 0) {
    if (vtab->release) {
      vtab->release(ptr);
    }
    xFree(ptr);
  }
}

void xCopy(void *ptr, void *other) {
  Header  *hdr;
  xVTable *vtab;

  hdr  = (Header *)ptr - 1;
  vtab = hdr->vtab;

  if (vtab->copy) {
    vtab->copy(ptr, other);
  }
}

void xMove(void *ptr, void *other) {
  Header  *hdr;
  xVTable *vtab;

  hdr  = (Header *)ptr - 1;
  vtab = hdr->vtab;

  if (vtab->move) {
    vtab->move(ptr, other);
  }
}
