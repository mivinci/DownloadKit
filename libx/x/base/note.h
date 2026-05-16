/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * note.h - Lightweight one-shot notification primitive
 *
 * xNote replaces per-object pthread_mutex_t + pthread_cond_t pairs for
 * the common "signal once, wait once" pattern.  It uses a three-level
 * wait strategy:
 *
 *   Level 1: Single atomic load (zero-cost fast path)
 *   Level 2: Brief spin with CPU pause/yield hints (~1μs)
 *   Level 3: Kernel-assisted wait (futex / __ulock / sched_yield)
 *
 * Compared to mutex+cond:
 *   - 4 bytes vs ~88 bytes (glibc)
 *   - Zero initialization, zero destruction
 *   - Fast path is a single atomic load (no syscall)
 */

#ifndef XBASE_NOTE_H
#define XBASE_NOTE_H

#include <x/base/atomic.h>
#include <x/base/base.h>

#if defined(__linux__)
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <os/lock.h>
#include <unistd.h>
/* Darwin private API used by libdispatch for lightweight waits. */
extern int __ulock_wait(uint32_t op, void *addr, uint64_t val, uint32_t timeout);
extern int __ulock_wake(uint32_t op, void *addr, uint64_t val);
#define X_UL_COMPARE_AND_WAIT 1
#define X_ULF_WAKE_ALL        0x00000100
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#endif

/**
 * @brief Lightweight one-shot notification.
 *
 * Embed in your struct and use xNoteSignal / xNoteWait for
 * single-producer single-consumer completion notification.
 * Zero-initialized state means "pending".
 */
XDEF_STRUCT(xNote) {
  int state; /**< 0 = pending, 1 = signaled */
};

/** @brief Static initializer for xNote. */
#define X_NOTE_INIT {0}

/**
 * @brief Initialize a note to the pending state.
 * @param n Pointer to the note.
 */
static inline void xNoteInit(xNote *n) {
  xAtomicStore(&n->state, 0, xAtomicRelaxed);
}

/**
 * @brief Check whether the note has been signaled (non-blocking).
 * @param n Pointer to the note.
 * @return true if signaled, false if still pending.
 */
static inline bool xNoteDone(const xNote *n) {
  return xAtomicLoad(&n->state, xAtomicAcquire) != 0;
}

/**
 * @brief Signal the note and wake any waiting thread.
 *
 * Must be called exactly once.  After this call, xNoteWait() and
 * xNoteDone() will return immediately.
 *
 * @param n Pointer to the note.
 */
static inline void xNoteSignal(xNote *n) {
  xAtomicStore(&n->state, 1, xAtomicRelease);

#if defined(__linux__)
  /* futex wake — no-op if nobody is waiting (doesn't enter kernel). */
  syscall(SYS_futex, &n->state, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
#elif defined(__APPLE__)
  __ulock_wake(X_UL_COMPARE_AND_WAIT | X_ULF_WAKE_ALL, &n->state, 0);
#elif defined(_WIN32)
  /* WakeByAddressAll requires all waiters to be in WakeByAddressSingleWait
   * — since our Level 3 uses SwitchToThread, no explicit wake needed. */
#endif
  /* Other platforms: no kernel wake needed; waiter uses sched_yield(). */
}

/**
 * @brief Block until the note is signaled.
 *
 * Uses a three-level strategy:
 *   1. Fast atomic check (covers the common case where signal
 *      has already been sent before the waiter arrives).
 *   2. Brief spin with architecture-specific pause hints.
 *   3. Kernel-assisted wait for the rare slow path.
 *
 * @param n Pointer to the note.
 */
static inline void xNoteWait(xNote *n) {
  /* Level 1: fast check — covers the overwhelmingly common case
   * (e.g. event-loop offload where the task finished long ago). */
  if (xAtomicLoad(&n->state, xAtomicAcquire) != 0) return;

  /* Level 2: brief spin with pause/yield hints (~1μs). */
  for (int i = 0; i < 64; i++) {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
#if defined(_MSC_VER)
    _mm_pause();
#else
    __builtin_ia32_pause();
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield");
#endif
    if (xAtomicLoad(&n->state, xAtomicAcquire) != 0) return;
  }

  /* Level 3: kernel-assisted wait. */
#if defined(__linux__)
  while (xAtomicLoad(&n->state, xAtomicAcquire) == 0) {
    syscall(SYS_futex, &n->state, FUTEX_WAIT, 0, NULL, NULL, 0);
  }
#elif defined(__APPLE__)
  while (xAtomicLoad(&n->state, xAtomicAcquire) == 0) {
    __ulock_wait(X_UL_COMPARE_AND_WAIT, &n->state, 0, 0);
  }
#elif defined(_WIN32)
  /* Windows fallback: yield loop. SwitchToThread() is equivalent to
   * sched_yield() — yields the remainder of the current time slice. */
  while (xAtomicLoad(&n->state, xAtomicAcquire) == 0) {
    SwitchToThread();
  }
#else
  /* Portable fallback: yield loop. */
  while (xAtomicLoad(&n->state, xAtomicAcquire) == 0) {
    sched_yield();
  }
#endif
}

#endif /* XBASE_NOTE_H */
