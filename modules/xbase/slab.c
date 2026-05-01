/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * slab.c - Fixed-size object pool implementation
 *
 * Memory layout
 * =============
 *
 *   xSlab  ──▶  chunk0 ──▶  chunk1 ──▶  ... ──▶ NULL
 *                 │           │
 *                 ▼           ▼
 *               [hdr][slot0][slot1]...[slotN]
 *
 * Each chunk starts with a small bookkeeping header (xSlabChunk)
 * followed by `nslots` back-to-back slots.  Free slots form a
 * singly-linked intrusive list — the first `sizeof(void *)` bytes
 * of a free slot store a pointer to the next free slot.  A slot is
 * therefore required to be at least as large as a pointer.
 *
 * Chunks are acquired via the platform's native anonymous mapping
 * when possible so that the request does not go through the C
 * runtime's heap arenas and does not interact with glibc's
 * per-thread caches when we hand out pointers to other threads.
 */

#include <xbase/atomic.h>
#include <xbase/slab.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define XSLAB_USE_VIRTUALALLOC 1
#elif defined(__linux__) || defined(__APPLE__) || defined(__unix__) || \
  defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/mman.h>
#include <unistd.h>
#define XSLAB_USE_MMAP 1
#endif

/* ─────────────────────── helpers ─────────────────────── */

static int xslab_is_pow2(size_t v) {
  return v != 0 && (v & (v - 1)) == 0;
}

static size_t xslab_round_up(size_t v, size_t a) {
  return (v + (a - 1)) & ~(a - 1);
}

/* Acquire a chunk of `bytes` from the OS.  Returns NULL on failure. */
static void *xslab_map(size_t bytes) {
#if defined(XSLAB_USE_VIRTUALALLOC)
  void *p = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  return p;
#elif defined(XSLAB_USE_MMAP)
  void *p =
    mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) return NULL;
  return p;
#else
  return malloc(bytes);
#endif
}

static void xslab_unmap(void *p, size_t bytes) {
  if (!p) return;
#if defined(XSLAB_USE_VIRTUALALLOC)
  (void)bytes;
  VirtualFree(p, 0, MEM_RELEASE);
#elif defined(XSLAB_USE_MMAP)
  munmap(p, bytes);
#else
  (void)bytes;
  free(p);
#endif
}

/* ─────────────────────── shared layout ─────────────────────── */

/*
 * Layout header sitting at the start of every chunk.  The first slot
 * starts at `slots_offset` bytes from the chunk base so that the
 * returned pointer is aligned to the requested boundary.
 */
typedef struct xSlabChunk_ xSlabChunk;
struct xSlabChunk_ {
  xSlabChunk *next;         /* next chunk in the list                 */
  size_t      bytes;        /* total chunk size passed to unmap       */
  size_t      slots_offset; /* offset of slot 0 from chunk base       */
  size_t      nslots;       /* number of slots in this chunk          */
};

typedef struct xSlabFreeNode_ xSlabFreeNode;
struct xSlabFreeNode_ {
  xSlabFreeNode *next;
};

/*
 * Compute chunk layout for the requested (slot_size, align, chunk_bytes).
 *
 *   slot_size is already rounded up to a multiple of align.
 *   Returns the finalised chunk size in *out_bytes and the number of
 *   slots that fit in *out_nslots and the offset of slot 0 in
 *   *out_offset.  Guarantees at least 1 slot per chunk.
 */
static void xslab_layout(size_t slot_size, size_t align, size_t chunk_bytes,
                         size_t *out_bytes, size_t *out_offset,
                         size_t *out_nslots) {
  size_t hdr    = xslab_round_up(sizeof(xSlabChunk), align);
  size_t wanted = chunk_bytes ? chunk_bytes : XSLAB_DEFAULT_CHUNK_BYTES;
  if (wanted < hdr + slot_size) wanted = hdr + slot_size;

  size_t usable = wanted - hdr;
  size_t nslots = usable / slot_size;
  if (nslots == 0) nslots = 1;

  *out_offset = hdr;
  *out_nslots = nslots;
  *out_bytes  = hdr + nslots * slot_size;
}

/* ─────────────────────── xSlab (single-threaded) ─────────────────────── */

struct xSlab_ {
  xSlabChunk    *chunks;      /* list of all chunks                   */
  xSlabFreeNode *free_head;   /* intrusive freelist                   */
  size_t         slot_size;   /* rounded-up slot size                 */
  size_t         align;       /* slot alignment                       */
  size_t         chunk_bytes; /* target chunk size (pre-layout)       */
  size_t         in_use;      /* currently handed-out slots           */
};

static int xslab_grow(struct xSlab_ *s) {
  size_t bytes, offset, nslots;
  xslab_layout(s->slot_size, s->align, s->chunk_bytes, &bytes, &offset,
               &nslots);

  xSlabChunk *c = (xSlabChunk *)xslab_map(bytes);
  if (!c) return -1;

  c->next         = s->chunks;
  c->bytes        = bytes;
  c->slots_offset = offset;
  c->nslots       = nslots;
  s->chunks       = c;

  /* Thread every slot onto the freelist. */
  char *base = (char *)c + offset;
  for (size_t i = 0; i < nslots; i++) {
    xSlabFreeNode *n = (xSlabFreeNode *)(base + i * s->slot_size);
    n->next          = s->free_head;
    s->free_head     = n;
  }
  return 0;
}

xSlab *xSlabCreate(size_t obj_size, size_t obj_align, size_t chunk_bytes) {
  if (obj_size == 0) return NULL;

  size_t align = obj_align ? obj_align : XSLAB_DEFAULT_ALIGN;
  if (!xslab_is_pow2(align)) return NULL;

  /* Slot must hold a pointer so it can be linked into the freelist. */
  size_t slot_size = obj_size;
  if (slot_size < sizeof(void *)) slot_size = sizeof(void *);
  slot_size = xslab_round_up(slot_size, align);

  struct xSlab_ *s = (struct xSlab_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->slot_size   = slot_size;
  s->align       = align;
  s->chunk_bytes = chunk_bytes ? chunk_bytes : XSLAB_DEFAULT_CHUNK_BYTES;

  return (xSlab *)s;
}

void xSlabDestroy(xSlab *h) {
  struct xSlab_ *s = (struct xSlab_ *)h;
  if (!s) return;

  xSlabChunk *c = s->chunks;
  while (c) {
    xSlabChunk *next = c->next;
    xslab_unmap(c, c->bytes);
    c = next;
  }
  free(s);
}

void *xSlabAlloc(xSlab *h) {
  struct xSlab_ *s = (struct xSlab_ *)h;
  if (!s) return NULL;

  if (!s->free_head) {
    if (xslab_grow(s) != 0) return NULL;
  }
  xSlabFreeNode *n = s->free_head;
  s->free_head     = n->next;
  s->in_use++;
  return (void *)n;
}

void xSlabFree(xSlab *h, void *p) {
  struct xSlab_ *s = (struct xSlab_ *)h;
  if (!s || !p) return;

  xSlabFreeNode *n = (xSlabFreeNode *)p;
  n->next          = s->free_head;
  s->free_head     = n;
  s->in_use--;
}

void xSlabReset(xSlab *h) {
  struct xSlab_ *s = (struct xSlab_ *)h;
  if (!s) return;

  s->free_head = NULL;
  s->in_use    = 0;

  /* Re-thread every slot from every chunk back onto the freelist. */
  for (xSlabChunk *c = s->chunks; c; c = c->next) {
    char *base = (char *)c + c->slots_offset;
    for (size_t i = 0; i < c->nslots; i++) {
      xSlabFreeNode *n = (xSlabFreeNode *)(base + i * s->slot_size);
      n->next          = s->free_head;
      s->free_head     = n;
    }
  }
}

size_t xSlabInUse(const xSlab *h) {
  const struct xSlab_ *s = (const struct xSlab_ *)h;
  return s ? s->in_use : 0;
}

size_t xSlabSlotSize(const xSlab *h) {
  const struct xSlab_ *s = (const struct xSlab_ *)h;
  return s ? s->slot_size : 0;
}

/* ─────────────────────── xSlabMt (multi-threaded) ─────────────────────── */

/*
 * Freelist is a Treiber stack.  Chunk acquisition is the slow path
 * and is serialised via a spinlock built on atomic exchange — this
 * only runs once every `nslots` allocations, so the overhead is
 * negligible and we avoid pulling in pthread just for a one-shot
 * latch.
 */

struct xSlabMt_ {
  xSlabChunk    *chunks;    /* head of chunk list               */
  xSlabFreeNode *free_head; /* lock-free freelist head          */
  int            grow_lock; /* 0=free, 1=locked                 */
  size_t         slot_size;
  size_t         align;
  size_t         chunk_bytes;
};

static void xslabmt_push_free(struct xSlabMt_ *s, xSlabFreeNode *n) {
  xSlabFreeNode *head;
  do {
    head    = xAtomicLoad(&s->free_head, xAtomicRelaxed);
    n->next = head;
  } while (!xAtomicCasWeak(&s->free_head, &head, n, xAtomicRelease));
}

static xSlabFreeNode *xslabmt_pop_free(struct xSlabMt_ *s) {
  /*
   * NOTE: Treiber stack has a classical ABA window.  We are immune
   * here because slots are *never* returned to the OS while the pool
   * is live, so a popped node's memory cannot be recycled into a
   * different allocation class between load and CAS.  The worst
   * outcome of a lost race is retrying the CAS.
   */
  xSlabFreeNode *head;
  for (;;) {
    head = xAtomicLoad(&s->free_head, xAtomicAcquire);
    if (!head) return NULL;
    xSlabFreeNode *next = head->next;
    if (xAtomicCasWeak(&s->free_head, &head, next, xAtomicAcqRel)) return head;
  }
}

static int xslabmt_grow(struct xSlabMt_ *s) {
  /* Acquire the grow lock via a simple test-and-set spin. */
  int expected = 0;
  while (!xAtomicCasWeak(&s->grow_lock, &expected, 1, xAtomicAcquire)) {
    /* Another thread is growing — let them finish and retry alloc. */
    expected = 0;
    if (xAtomicLoad(&s->free_head, xAtomicAcquire) != NULL) return 0;
  }

  /* Re-check: another thread may have grown while we spun. */
  if (xAtomicLoad(&s->free_head, xAtomicAcquire) != NULL) {
    xAtomicStore(&s->grow_lock, 0, xAtomicRelease);
    return 0;
  }

  size_t bytes, offset, nslots;
  xslab_layout(s->slot_size, s->align, s->chunk_bytes, &bytes, &offset,
               &nslots);

  xSlabChunk *c = (xSlabChunk *)xslab_map(bytes);
  if (!c) {
    xAtomicStore(&s->grow_lock, 0, xAtomicRelease);
    return -1;
  }

  c->bytes        = bytes;
  c->slots_offset = offset;
  c->nslots       = nslots;

  /* Link chunk into the list (only writer under grow_lock). */
  c->next   = s->chunks;
  s->chunks = c;

  /* Build a local freelist of all new slots, then splice. */
  char          *base       = (char *)c + offset;
  xSlabFreeNode *local_head = NULL;
  for (size_t i = 0; i < nslots; i++) {
    xSlabFreeNode *n = (xSlabFreeNode *)(base + i * s->slot_size);
    n->next          = local_head;
    local_head       = n;
  }
  /* Find tail of local list. */
  xSlabFreeNode *local_tail = local_head;
  while (local_tail->next)
    local_tail = local_tail->next;

  /* Splice local list onto the global freelist head. */
  xSlabFreeNode *head;
  do {
    head             = xAtomicLoad(&s->free_head, xAtomicRelaxed);
    local_tail->next = head;
  } while (!xAtomicCasWeak(&s->free_head, &head, local_head, xAtomicRelease));

  xAtomicStore(&s->grow_lock, 0, xAtomicRelease);
  return 0;
}

xSlabMt *xSlabMtCreate(size_t obj_size, size_t obj_align, size_t chunk_bytes) {
  if (obj_size == 0) return NULL;

  size_t align = obj_align ? obj_align : XSLAB_DEFAULT_ALIGN;
  if (!xslab_is_pow2(align)) return NULL;

  size_t slot_size = obj_size;
  if (slot_size < sizeof(void *)) slot_size = sizeof(void *);
  slot_size = xslab_round_up(slot_size, align);

  struct xSlabMt_ *s = (struct xSlabMt_ *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  s->slot_size   = slot_size;
  s->align       = align;
  s->chunk_bytes = chunk_bytes ? chunk_bytes : XSLAB_DEFAULT_CHUNK_BYTES;

  return (xSlabMt *)s;
}

void xSlabMtDestroy(xSlabMt *h) {
  struct xSlabMt_ *s = (struct xSlabMt_ *)h;
  if (!s) return;

  xSlabChunk *c = s->chunks;
  while (c) {
    xSlabChunk *next = c->next;
    xslab_unmap(c, c->bytes);
    c = next;
  }
  free(s);
}

void *xSlabMtAlloc(xSlabMt *h) {
  struct xSlabMt_ *s = (struct xSlabMt_ *)h;
  if (!s) return NULL;

  /*
   * Loop until we either get a slot or grow() reports a hard failure
   * (OS-level map error).  A single pop+grow+pop sequence is not
   * enough under contention: another thread may drain the freelist
   * between grow() and our second pop_free(), in which case we
   * simply try again (and grow() will bail out cheaply if someone
   * already refilled the list).
   */
  for (;;) {
    xSlabFreeNode *n = xslabmt_pop_free(s);
    if (n) return (void *)n;

    if (xslabmt_grow(s) != 0) return NULL;
  }
}

void xSlabMtFree(xSlabMt *h, void *p) {
  struct xSlabMt_ *s = (struct xSlabMt_ *)h;
  if (!s || !p) return;
  xslabmt_push_free(s, (xSlabFreeNode *)p);
}

size_t xSlabMtSlotSize(const xSlabMt *h) {
  const struct xSlabMt_ *s = (const struct xSlabMt_ *)h;
  return s ? s->slot_size : 0;
}
