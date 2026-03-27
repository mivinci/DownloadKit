/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * atomic.h - Atomic operations
 */

#ifndef XBASE_ATOMIC_H
#define XBASE_ATOMIC_H

#define xAtomicAcquire __ATOMIC_ACQUIRE
#define xAtomicRelease __ATOMIC_RELEASE
#define xAtomicSeqCst  __ATOMIC_SEQ_CST
#define xAtomicRelaxed __ATOMIC_RELAXED
#define xAtomicConsume __ATOMIC_CONSUME
#define xAtomicAcqRel  __ATOMIC_ACQ_REL

#define xAtomicLoad(p, o)            __atomic_load_n(p, o)
#define xAtomicStore(p, v, o)        __atomic_store_n(p, v, o)
#define xAtomicXchg(p, v, o)         __atomic_exchange_n(p, v, o)
#define xAtomicCasWeak(p, e, d, o)   xAtomicCas(p, e, d, true, o)
#define xAtomicCasStrong(p, e, d, o) xAtomicCas(p, e, d, false, o)
#define xAtomicCas(p, e, d, w, o)                                              \
  __atomic_compare_exchange_n(p, e, d, w, o, xAtomicRelaxed)

#define xAtomicAdd(p, v, o)         __atomic_add_fetch(p, v, o)
#define xAtomicSub(p, v, o)         __atomic_sub_fetch(p, v, o)
#define xAtomicAnd(p, v, o)         __atomic_and_fetch(p, v, o)
#define xAtomicOr(p, v, o)          __atomic_or_fetch(p, v, o)
#define xAtomicXor(p, v, o)         __atomic_xor_fetch(p, v, o)
#define xAtomicNand(p, v, o)        __atomic_nand_fetch(p, v, o)
#define xAtomicFetchAdd(p, v, o)    __atomic_fetch_add(p, v, o)
#define xAtomicFetchSub(p, v, o)    __atomic_fetch_sub(p, v, o)
#define xAtomicFetchAnd(p, v, o)    __atomic_fetch_and(p, v, o)
#define xAtomicFetchOr(p, v, o)     __atomic_fetch_or(p, v, o)
#define xAtomicFetchXor(p, v, o)    __atomic_fetch_xor(p, v, o)

#endif // XBASE_ATOMIC_H
