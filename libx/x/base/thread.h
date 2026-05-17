/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * thread.h - Cross-platform threading primitives.
 *
 * Provides mutex, condition variable, thread, and one-time-init
 * abstractions over pthreads (POSIX) and Win32 APIs. Win 7 / Vista+
 * compatible.
 *
 * Public API: function names start with x; type names are xMutex,
 * xCond, xThread, xOnce. Implementations live in thread.c so this
 * header is light and the platform-specific glue (Win32 callbacks,
 * etc.) doesn't bleed into every TU that takes a lock.
 *
 * NOTE: <windows.h> is still pulled in by this header on Windows
 * because xMutex/xCond/xThread/xOnce typedef to platform types. If
 * a TU wants to dodge windows.h's macro pollution it should include
 * thread.h before any of its own headers, or wrap the lock/cond
 * carriers in heap-allocated shims.
 */

#ifndef X_BASE_THREAD_H
#define X_BASE_THREAD_H

#include <x/base/base.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE             xThread;
typedef CRITICAL_SECTION   xMutex;
typedef CONDITION_VARIABLE xCond;
typedef INIT_ONCE          xOnce;

#define X_ONCE_INIT INIT_ONCE_STATIC_INIT

#else /* _WIN32 */

#include <pthread.h>

typedef pthread_t       xThread;
typedef pthread_mutex_t xMutex;
typedef pthread_cond_t  xCond;
typedef pthread_once_t  xOnce;

#define X_ONCE_INIT PTHREAD_ONCE_INIT

#endif /* _WIN32 / POSIX */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Mutex ─────────────────────────────────────────────────────────── */

/** Initialise an embedded xMutex. Must be paired with xMutexDestroy. */
void xMutexInit(xMutex *m);

/** Release the resources held by @p m. The mutex must be unlocked. */
void xMutexDestroy(xMutex *m);

/** Acquire the lock; blocks until exclusive ownership is granted. */
void xMutexLock(xMutex *m);

/**
 * @brief Try to acquire the lock without blocking.
 * @return 0 on success, non-zero if the mutex is already held.
 */
int xMutexTryLock(xMutex *m);

/** Release the lock. Caller must currently own it. */
void xMutexUnlock(xMutex *m);

/* ── Condition variable ────────────────────────────────────────────── */

/** Initialise an embedded xCond. Pair with xCondDestroy. */
void xCondInit(xCond *c);

/** Release the resources held by @p c. */
void xCondDestroy(xCond *c);

/** Wake one thread waiting on @p c (or none, if there are no waiters). */
void xCondSignal(xCond *c);

/** Wake every thread waiting on @p c. */
void xCondBroadcast(xCond *c);

/**
 * @brief Atomically release @p m and wait on @p c. On wakeup,
 *        re-acquire @p m before returning.
 *
 * Spurious wakeups are possible — callers should always re-check the
 * condition under the lock.
 */
void xCondWait(xCond *c, xMutex *m);

/**
 * @brief Like xCondWait but bounded by an absolute timeout in ms.
 *
 * @param timeout_ms  Wait at most this many milliseconds before
 *                    returning. Use 0 for an immediate poll.
 * @return            0 if signalled, 1 if timed out, -1 on error.
 *                    The mutex is re-acquired in all cases.
 */
int xCondTimedWait(xCond *c, xMutex *m, unsigned timeout_ms);

/* ── Thread ────────────────────────────────────────────────────────── */

/** Spawn a thread running @p fn(arg). Returns 0 on success. */
int xThreadCreate(xThread *t, void *(*fn)(void *), void *arg);

/** Wait for @p t to exit. */
void xThreadJoin(xThread t);

/* ── One-time init ─────────────────────────────────────────────────── */

/**
 * @brief Run @p fn at most once across the lifetime of @p o.
 *
 * The first thread to call this with a given @p o runs @p fn; later
 * callers (and the same caller again) are no-ops. @p o must have
 * been initialised with X_ONCE_INIT.
 */
void xOnceCall(xOnce *o, void (*fn)(void));

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* X_BASE_THREAD_H */
