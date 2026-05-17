/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * cond.cpp - Out-of-line bridge between xpp::Condvar and libx's
 *            xCond.
 *
 * Kept out of cond.h so consumers don't transitively pick up
 * <pthread.h> / <windows.h>. The header exposes only an opaque
 * cond_storage byte buffer; the static_asserts below keep its size
 * and alignment honest against the real xCond on this platform.
 */

#include <xpp/cond.h>
#include <xpp/mutex.h>

extern "C" {
#include <x/base/thread.h>
}

namespace xpp {
namespace _ {

static_assert(sizeof(xCond) <= k_cond_storage_size,
              "cond_storage too small for xCond on this platform — bump k_cond_storage_size");
static_assert(alignof(xCond) <= k_cond_storage_align,
              "cond_storage under-aligned for xCond — bump k_cond_storage_align");

static xCond *raw_cond(cond_storage *s) noexcept {
  return reinterpret_cast<xCond *>(s->buf);
}

static xMutex *raw_mutex(mutex_storage *s) noexcept {
  return reinterpret_cast<xMutex *>(s->buf);
}

void cond_init(cond_storage *s) noexcept {
  xCondInit(raw_cond(s));
}

void cond_destroy(cond_storage *s) noexcept {
  xCondDestroy(raw_cond(s));
}

void cond_signal(cond_storage *s) noexcept {
  xCondSignal(raw_cond(s));
}

void cond_broadcast(cond_storage *s) noexcept {
  xCondBroadcast(raw_cond(s));
}

void cond_wait(cond_storage *c, mutex_storage *m) noexcept {
  xCondWait(raw_cond(c), raw_mutex(m));
}

int cond_timed_wait(cond_storage *c, mutex_storage *m, unsigned timeout_ms) noexcept {
  return xCondTimedWait(raw_cond(c), raw_mutex(m), timeout_ms);
}

} // namespace _
} // namespace xpp
