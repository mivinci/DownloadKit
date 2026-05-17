/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex.cpp - Out-of-line bridge between xpp::Mutex<T> and libx's
 *             xMutex.
 *
 * Kept out of mutex.h so consumers of <xpp/mutex.h> don't
 * transitively pick up <pthread.h> on POSIX or <windows.h> on
 * Windows, and never see the C-level xMutex / xMutexLock / …
 * symbols. The header exposes only an opaque, correctly-sized
 * mutex_storage byte buffer; the static_asserts below are the
 * load-bearing piece that keeps "correctly-sized" honest.
 */

#include <xpp/mutex.h>

extern "C" {
#include <x/base/thread.h>
}

namespace xpp {
namespace _ {

static_assert(sizeof(xMutex) <= k_mutex_storage_size,
              "mutex_storage too small for xMutex on this platform — bump k_mutex_storage_size");
static_assert(alignof(xMutex) <= k_mutex_storage_align,
              "mutex_storage under-aligned for xMutex — bump k_mutex_storage_align");

static xMutex *raw(mutex_storage *s) noexcept {
  return reinterpret_cast<xMutex *>(s->buf);
}

void mutex_init(mutex_storage *s) noexcept {
  xMutexInit(raw(s));
}

void mutex_destroy(mutex_storage *s) noexcept {
  xMutexDestroy(raw(s));
}

void mutex_lock(mutex_storage *s) noexcept {
  xMutexLock(raw(s));
}

int mutex_try_lock(mutex_storage *s) noexcept {
  return xMutexTryLock(raw(s));
}

void mutex_unlock(mutex_storage *s) noexcept {
  xMutexUnlock(raw(s));
}

} // namespace _
} // namespace xpp
