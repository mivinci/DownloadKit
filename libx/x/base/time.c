/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * time.c - Time utilities
 */

#include <x/base/time.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t xMonoMs(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER        count;

  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);

  QueryPerformanceCounter(&count);
  /* Convert to milliseconds without overflow:
   *   count / freq * 1000  =>  (count * 1000) / freq
   * For large counts we do it in two steps to avoid 128-bit math. */
  return (uint64_t)(count.QuadPart / freq.QuadPart) * 1000u +
         (uint64_t)((count.QuadPart % freq.QuadPart) * 1000u / freq.QuadPart);
}

uint64_t xWallMs(void) {
  FILETIME       ft;
  ULARGE_INTEGER ul;

  GetSystemTimeAsFileTime(&ft);
  ul.LowPart  = ft.dwLowDateTime;
  ul.HighPart = ft.dwHighDateTime;

  /* FILETIME is 100-ns intervals since 1601-01-01 UTC.
   * Unix epoch (1970-01-01) is 11644473600 seconds later.
   * Convert to ms: (ticks / 10000) - epoch_offset_ms */
  return (ul.QuadPart / 10000u) - 11644473600000uLL;
}

#else /* POSIX */

#include <time.h>

uint64_t xMonoMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t xWallMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

#endif /* _WIN32 / POSIX */
