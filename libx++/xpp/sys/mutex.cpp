/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex.cpp - Out-of-line bridge between xpp::Mutex<T> and libx's
 *             xMutex, plus debug-mode deadlock detection.
 *
 * Kept out of mutex.h so consumers of <xpp/mutex.h> don't
 * transitively pick up <pthread.h> on POSIX or <windows.h> on
 * Windows, and never see the C-level xMutex / xMutexLock / …
 * symbols. The header exposes only an opaque, correctly-sized
 * mutex_storage byte buffer; the static_asserts below are the
 * load-bearing piece that keeps "correctly-sized" honest.
 *
 * Deadlock detection (XPP_DEBUG only):
 * Each mutex gets a unique monotonic ID at init time. Each thread
 * maintains a stack of held lock IDs. On lock(), we verify:
 *   1. Not already held (reentrant deadlock)
 *   2. ID > all currently held IDs (lock ordering violation)
 * This catches potential deadlocks at the first out-of-order
 * acquisition, before the actual hang occurs.
 */

#include <xpp/sys/mutex.h>

extern "C" {
#include <x/base/thread.h>
}

#if XPP_DEBUG
#include <atomic>
#endif

namespace xpp {
namespace sys {
namespace _ {

static_assert(sizeof(xMutex) <= k_mutex_storage_size,
              "mutex_storage too small for xMutex on this platform — bump k_mutex_storage_size");
static_assert(alignof(xMutex) <= k_mutex_storage_align,
              "mutex_storage under-aligned for xMutex — bump k_mutex_storage_align");

static xMutex *raw(mutex_storage *s) noexcept {
  return reinterpret_cast<xMutex *>(s->buf);
}

/* ── Deadlock detection (debug only) ──────────────────────────────── */

#if XPP_DEBUG

namespace deadlock {

static std::atomic<uint64_t> g_next_id{1};

/** Maximum nesting depth we track. Exceeding this is itself a bug. */
static constexpr size_t kMaxHeldLocks = 32;

struct ThreadState {
  uint64_t held[kMaxHeldLocks];
  size_t   depth = 0;
};

static thread_local ThreadState t_state;

static uint64_t alloc_id() {
  return g_next_id.fetch_add(1, std::memory_order_relaxed);
}

static void on_lock(uint64_t id) {
  auto &ts = t_state;

  /* Check reentrant: same lock acquired twice on this thread. */
  for (size_t i = 0; i < ts.depth; ++i) {
    XPP_ASSERT(ts.held[i] != id, "deadlock: mutex %llu locked twice on the same thread (reentrant)",
               (unsigned long long)id);
  }

  /* Check ordering: new lock must have a larger ID than all held locks.
   * If not, two threads could be acquiring these in opposite order. */
  if (ts.depth > 0) {
    uint64_t top = ts.held[ts.depth - 1];
    XPP_ASSERT(id > top,
               "deadlock: lock order violation — acquiring mutex %llu while holding %llu "
               "(locks must be acquired in ascending ID order)",
               (unsigned long long)id, (unsigned long long)top);
  }

  /* Push onto held stack. */
  XPP_ASSERT(ts.depth < kMaxHeldLocks, "deadlock: too many nested locks (%zu), likely a bug",
             ts.depth);
  ts.held[ts.depth++] = id;
}

static void on_unlock(uint64_t id) {
  auto &ts = t_state;
  XPP_DEBUG_ASSERT(ts.depth > 0, "deadlock: unlock with empty held-stack");
  XPP_DEBUG_ASSERT(ts.held[ts.depth - 1] == id,
                   "deadlock: unlock order mismatch — expected mutex %llu, got %llu",
                   (unsigned long long)ts.held[ts.depth - 1], (unsigned long long)id);
  --ts.depth;
}

} // namespace deadlock

#endif // XPP_DEBUG

/* ── Bridge functions ─────────────────────────────────────────────── */

void mutex_init(mutex_storage *s) noexcept {
  xMutexInit(raw(s));
#if XPP_DEBUG
  s->lock_id = deadlock::alloc_id();
#endif
}

void mutex_destroy(mutex_storage *s) noexcept {
  xMutexDestroy(raw(s));
}

void mutex_lock(mutex_storage *s) noexcept {
#if XPP_DEBUG
  deadlock::on_lock(s->lock_id);
#endif
  xMutexLock(raw(s));
}

int mutex_try_lock(mutex_storage *s) noexcept {
  int rc = xMutexTryLock(raw(s));
#if XPP_DEBUG
  if (rc == 0) {
    /* Successfully acquired — register in held-stack. */
    deadlock::on_lock(s->lock_id);
  }
#endif
  return rc;
}

void mutex_unlock(mutex_storage *s) noexcept {
  xMutexUnlock(raw(s));
#if XPP_DEBUG
  deadlock::on_unlock(s->lock_id);
#endif
}

} // namespace _
} // namespace sys
} // namespace xpp
