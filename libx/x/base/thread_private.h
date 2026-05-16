/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * thread_private.h - Platform-agnostic threading primitives (internal)
 *
 * Provides mutex, condition variable, thread, and one-time init
 * abstractions over pthreads (POSIX) and Win32 APIs.
 * Windows 7 / Vista+ compatible.
 */

#ifndef XBASE_THREAD_PRIVATE_H
#define XBASE_THREAD_PRIVATE_H

#include <x/base/base.h>

/* ──────────────────── Win32 ──────────────────── */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HANDLE             xThread;
typedef CRITICAL_SECTION   xMutex;
typedef CONDITION_VARIABLE xCond;
typedef INIT_ONCE          xOnce;

#define X_ONCE_INIT INIT_ONCE_STATIC_INIT

static inline void xMutexInit(xMutex *m) {
  InitializeCriticalSection(m);
}

static inline void xMutexDestroy(xMutex *m) {
  DeleteCriticalSection(m);
}

static inline void xMutexLock(xMutex *m) {
  EnterCriticalSection(m);
}

static inline void xMutexUnlock(xMutex *m) {
  LeaveCriticalSection(m);
}

static inline void xCondInit(xCond *c) {
  InitializeConditionVariable(c);
}

static inline void xCondDestroy(xCond *c) {
  (void)c; /* CONDITION_VARIABLE needs no destruction */
}

static inline void xCondSignal(xCond *c) {
  WakeConditionVariable(c);
}

static inline void xCondBroadcast(xCond *c) {
  WakeAllConditionVariable(c);
}

static inline void xCondWait(xCond *c, xMutex *m) {
  SleepConditionVariableCS(c, m, INFINITE);
}

static inline int xThreadCreate(xThread *t,
                                 void *(*fn)(void *), void *arg) {
  *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
  return *t ? 0 : -1;
}

static inline void xThreadJoin(xThread t) {
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}

static BOOL CALLBACK _xonce_trampoline(PINIT_ONCE o, PVOID param,
                                        PVOID *ctx) {
  ((void (*)(void))param)();
  return TRUE;
}

static inline void xOnceCall(xOnce *o, void (*fn)(void)) {
  InitOnceExecuteOnce(o, _xonce_trampoline, (PVOID)fn, NULL);
}

/* ──────────────────── POSIX ──────────────────── */

#else /* _WIN32 */

#include <pthread.h>

typedef pthread_t        xThread;
typedef pthread_mutex_t  xMutex;
typedef pthread_cond_t   xCond;
typedef pthread_once_t   xOnce;

#define X_ONCE_INIT PTHREAD_ONCE_INIT

static inline void xMutexInit(xMutex *m) {
  pthread_mutex_init(m, NULL);
}

static inline void xMutexDestroy(xMutex *m) {
  pthread_mutex_destroy(m);
}

static inline void xMutexLock(xMutex *m) {
  pthread_mutex_lock(m);
}

static inline void xMutexUnlock(xMutex *m) {
  pthread_mutex_unlock(m);
}

static inline void xCondInit(xCond *c) {
  pthread_cond_init(c, NULL);
}

static inline void xCondDestroy(xCond *c) {
  pthread_cond_destroy(c);
}

static inline void xCondSignal(xCond *c) {
  pthread_cond_signal(c);
}

static inline void xCondBroadcast(xCond *c) {
  pthread_cond_broadcast(c);
}

static inline void xCondWait(xCond *c, xMutex *m) {
  pthread_cond_wait(c, m);
}

static inline int xThreadCreate(xThread *t,
                                 void *(*fn)(void *), void *arg) {
  return pthread_create(t, NULL, fn, arg);
}

static inline void xThreadJoin(xThread t) {
  pthread_join(t, NULL);
}

static inline void xOnceCall(xOnce *o, void (*fn)(void)) {
  pthread_once(o, fn);
}

#endif /* _WIN32 / POSIX */

#endif /* XBASE_THREAD_PRIVATE_H */
