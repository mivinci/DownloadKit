/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * thread.c - Cross-platform threading primitives implementation.
 *
 * Public API in <x/base/thread.h>. Win32 path uses CRITICAL_SECTION,
 * SleepConditionVariableCS, and InitOnceExecuteOnce; POSIX path uses
 * pthread mutex / cond / once.
 */

#include <x/base/thread.h>

#ifdef _WIN32

void xMutexInit(xMutex *m) {
  InitializeCriticalSection(m);
}

void xMutexDestroy(xMutex *m) {
  DeleteCriticalSection(m);
}

void xMutexLock(xMutex *m) {
  EnterCriticalSection(m);
}

int xMutexTryLock(xMutex *m) {
  return TryEnterCriticalSection(m) ? 0 : 1;
}

void xMutexUnlock(xMutex *m) {
  LeaveCriticalSection(m);
}

void xCondInit(xCond *c) {
  InitializeConditionVariable(c);
}

void xCondDestroy(xCond *c) {
  (void) c; /* CONDITION_VARIABLE needs no destruction */
}

void xCondSignal(xCond *c) {
  WakeConditionVariable(c);
}

void xCondBroadcast(xCond *c) {
  WakeAllConditionVariable(c);
}

void xCondWait(xCond *c, xMutex *m) {
  SleepConditionVariableCS(c, m, INFINITE);
}

int xCondTimedWait(xCond *c, xMutex *m, unsigned timeout_ms) {
  if (SleepConditionVariableCS(c, m, (DWORD) timeout_ms)) return 0;
  if (GetLastError() == ERROR_TIMEOUT) return 1;
  return -1;
}

int xThreadCreate(xThread *t, void *(*fn)(void *), void *arg) {
  *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) fn, arg, 0, NULL);
  return *t ? 0 : -1;
}

void xThreadJoin(xThread t) {
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}

static BOOL CALLBACK _xonce_trampoline(PINIT_ONCE o, PVOID param, PVOID *ctx) {
  (void) o;
  (void) ctx;
  ((void (*)(void)) param)();
  return TRUE;
}

void xOnceCall(xOnce *o, void (*fn)(void)) {
  InitOnceExecuteOnce(o, _xonce_trampoline, (PVOID) fn, NULL);
}

#else /* _WIN32 */

#include <errno.h>
#include <time.h>

void xMutexInit(xMutex *m) {
  pthread_mutex_init(m, NULL);
}

void xMutexDestroy(xMutex *m) {
  pthread_mutex_destroy(m);
}

void xMutexLock(xMutex *m) {
  pthread_mutex_lock(m);
}

int xMutexTryLock(xMutex *m) {
  return pthread_mutex_trylock(m); /* 0 ok, EBUSY=16 already held */
}

void xMutexUnlock(xMutex *m) {
  pthread_mutex_unlock(m);
}

void xCondInit(xCond *c) {
  pthread_cond_init(c, NULL);
}

void xCondDestroy(xCond *c) {
  pthread_cond_destroy(c);
}

void xCondSignal(xCond *c) {
  pthread_cond_signal(c);
}

void xCondBroadcast(xCond *c) {
  pthread_cond_broadcast(c);
}

void xCondWait(xCond *c, xMutex *m) {
  pthread_cond_wait(c, m);
}

int xCondTimedWait(xCond *c, xMutex *m, unsigned timeout_ms) {
  /* pthread_cond_timedwait wants an absolute CLOCK_REALTIME deadline.
   * Compute now + timeout_ms; on EAGAIN/ETIMEDOUT return 1. */
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += (time_t) (timeout_ms / 1000);
  ts.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1000000000L;
  }
  int rc = pthread_cond_timedwait(c, m, &ts);
  if (rc == 0) return 0;
  if (rc == ETIMEDOUT) return 1;
  return -1;
}

int xThreadCreate(xThread *t, void *(*fn)(void *), void *arg) {
  return pthread_create(t, NULL, fn, arg);
}

void xThreadJoin(xThread t) {
  pthread_join(t, NULL);
}

void xOnceCall(xOnce *o, void (*fn)(void)) {
  pthread_once(o, fn);
}

#endif /* _WIN32 / POSIX */
