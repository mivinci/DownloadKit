/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * atomic.h - Atomic operations
 */

#ifndef XBASE_ATOMIC_H
#define XBASE_ATOMIC_H

/* ──────────── Memory order constants ──────────── */

#define xAtomicRelaxed 0
#define xAtomicConsume 1
#define xAtomicAcquire 2
#define xAtomicRelease 3
#define xAtomicAcqRel  4
#define xAtomicSeqCst  5

/* ──────────── GCC / Clang: use __atomic builtins ──────────── */

#if defined(__GNUC__) || defined(__clang__)

/* Remap order constants to GCC built-in constants at usage sites. */
#define _XAO(o)                               \
  ((o) == xAtomicRelaxed   ? __ATOMIC_RELAXED \
   : (o) == xAtomicConsume ? __ATOMIC_CONSUME \
   : (o) == xAtomicAcquire ? __ATOMIC_ACQUIRE \
   : (o) == xAtomicRelease ? __ATOMIC_RELEASE \
   : (o) == xAtomicAcqRel  ? __ATOMIC_ACQ_REL \
                           : __ATOMIC_SEQ_CST)

#define xAtomicLoad(p, o)            __atomic_load_n(p, _XAO(o))
#define xAtomicStore(p, v, o)        __atomic_store_n(p, v, _XAO(o))
#define xAtomicXchg(p, v, o)         __atomic_exchange_n(p, v, _XAO(o))
#define xAtomicCasWeak(p, e, d, o)   xAtomicCas(p, e, d, true, o)
#define xAtomicCasStrong(p, e, d, o) xAtomicCas(p, e, d, false, o)
#define xAtomicCas(p, e, d, w, o) \
  __atomic_compare_exchange_n(p, e, d, w, _XAO(o), __ATOMIC_RELAXED)

#define xAtomicAdd(p, v, o)      __atomic_add_fetch(p, v, _XAO(o))
#define xAtomicSub(p, v, o)      __atomic_sub_fetch(p, v, _XAO(o))
#define xAtomicAnd(p, v, o)      __atomic_and_fetch(p, v, _XAO(o))
#define xAtomicOr(p, v, o)       __atomic_or_fetch(p, v, _XAO(o))
#define xAtomicXor(p, v, o)      __atomic_xor_fetch(p, v, _XAO(o))
#define xAtomicNand(p, v, o)     __atomic_nand_fetch(p, v, _XAO(o))
#define xAtomicFetchAdd(p, v, o) __atomic_fetch_add(p, v, _XAO(o))
#define xAtomicFetchSub(p, v, o) __atomic_fetch_sub(p, v, _XAO(o))
#define xAtomicFetchAnd(p, v, o) __atomic_fetch_and(p, v, _XAO(o))
#define xAtomicFetchOr(p, v, o)  __atomic_fetch_or(p, v, _XAO(o))
#define xAtomicFetchXor(p, v, o) __atomic_fetch_xor(p, v, _XAO(o))

/* Pointer-sized variants (same as the non-Ptr versions on GCC/Clang). */
#define xAtomicXchgPtr(p, v, o)         xAtomicXchg(p, v, o)
#define xAtomicCasPtrWeak(p, e, d, o)   xAtomicCasWeak(p, e, d, o)
#define xAtomicCasPtrStrong(p, e, d, o) xAtomicCasStrong(p, e, d, o)

/* ──────────── MSVC: use Interlocked* + compiler barriers ──────────── */

#elif defined(_MSC_VER)

#include <intrin.h>

/*
 * MSVC Interlocked* functions provide full memory barriers (seq-cst).
 * On x86/x64 this is essentially free, so we accept the slight
 * over-fencing on relaxed/acquire/release orders.  This keeps the
 * implementation correct and Windows 7 compatible.
 *
 * Pointers passed to these macros must point to naturally-aligned
 * LONG (32-bit) or __int64 (64-bit) values.
 */

#define xAtomicLoad(p, o)     (*(p))
#define xAtomicStore(p, v, o) (*(p) = (v))

static inline long _xInterlockedXchg(volatile long *p, long v) {
  return _InterlockedExchange(p, v);
}
#define xAtomicXchg(p, v, o)  _xInterlockedXchg((volatile long *)(p), (long)(v))

static inline long _xInterlockedCas(volatile long *p, long *e, long d) {
  long comparand = *e;
  long old = _InterlockedCompareExchange(p, d, comparand);
  if (old != comparand) { *e = old; return 0; }
  return 1;
}
#define xAtomicCasWeak(p, e, d, o) \
  _xInterlockedCas((volatile long *)(p), (long *)(e), (long)(d))
#define xAtomicCasStrong(p, e, d, o) \
  _xInterlockedCas((volatile long *)(p), (long *)(e), (long)(d))
#define xAtomicCas(p, e, d, w, o) \
  _xInterlockedCas((volatile long *)(p), (long *)(e), (long)(d))

static inline long _xInterlockedAdd(volatile long *p, long v) {
  return _InterlockedExchangeAdd(p, v) + v;
}
#define xAtomicAdd(p, v, o)  _xInterlockedAdd((volatile long *)(p), (long)(v))

static inline long _xInterlockedSub(volatile long *p, long v) {
  return _InterlockedExchangeAdd(p, -(long)(v)) - v;
}
#define xAtomicSub(p, v, o)  _xInterlockedSub((volatile long *)(p), (long)(v))

static inline long _xInterlockedAnd(volatile long *p, long v) {
  return _InterlockedAnd(p, v) & v;
}
#define xAtomicAnd(p, v, o)  _xInterlockedAnd((volatile long *)(p), (long)(v))

static inline long _xInterlockedOr(volatile long *p, long v) {
  return _InterlockedOr(p, v) | v;
}
#define xAtomicOr(p, v, o)   _xInterlockedOr((volatile long *)(p), (long)(v))

static inline long _xInterlockedXor(volatile long *p, long v) {
  return _InterlockedXor(p, v) ^ v;
}
#define xAtomicXor(p, v, o)  _xInterlockedXor((volatile long *)(p), (long)(v))

/* NAND: ~(old & v).  Interlocked NAND not available on Win7, compose it. */
static inline long _xInterlockedNand(volatile long *p, long v) {
  long old, next;
  do {
    old  = *p;
    next = ~(old & v);
  } while (_InterlockedCompareExchange(p, next, old) != old);
  return ~next;
}
#define xAtomicNand(p, v, o) _xInterlockedNand((volatile long *)(p), (long)(v))

#define xAtomicFetchAdd(p, v, o) \
  _InterlockedExchangeAdd((volatile long *)(p), (long)(v))
#define xAtomicFetchSub(p, v, o) \
  _InterlockedExchangeAdd((volatile long *)(p), -(long)(v))
#define xAtomicFetchAnd(p, v, o) \
  _InterlockedAnd((volatile long *)(p), (long)(v))
#define xAtomicFetchOr(p, v, o) _InterlockedOr((volatile long *)(p), (long)(v))
#define xAtomicFetchXor(p, v, o) \
  _InterlockedXor((volatile long *)(p), (long)(v))

/* ──────── Pointer-sized atomics (for void*, struct*, etc.) ──────── */

static inline void *_xInterlockedXchgPtr(void *volatile *p, void *v) {
  return _InterlockedExchangePointer(p, v);
}
#define xAtomicXchgPtr(p, v, o) \
  _xInterlockedXchgPtr((void *volatile *)(p), (void *)(v))

static inline int _xInterlockedCasPtr(void *volatile *p, void **e, void *d) {
  void *comparand = *e;
  void *old = _InterlockedCompareExchangePointer(p, d, comparand);
  if (old != comparand) { *e = old; return 0; }
  return 1;
}
#define xAtomicCasPtrWeak(p, e, d, o) \
  _xInterlockedCasPtr((void *volatile *)(p), (void **)(e), (void *)(d))
#define xAtomicCasPtrStrong(p, e, d, o) \
  _xInterlockedCasPtr((void *volatile *)(p), (void **)(e), (void *)(d))

#else
#error "Unsupported compiler for xbase atomic operations"
#endif

#endif // XBASE_ATOMIC_H
