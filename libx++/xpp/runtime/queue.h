/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * queue.h - Lock-free SPSC work-stealing queue.
 *
 * Modeled after Go's P-local runqueue: a power-of-two array with
 * atomic head/tail indices.
 *
 *   Owner (the worker thread): push() / pop() at head (LIFO).
 *   Thieves (other workers):   steal() from tail (FIFO).
 *
 * If full, push() returns false — caller overflows to global queue.
 */

#ifndef XPP_RUNTIME_QUEUE_H
#define XPP_RUNTIME_QUEUE_H

#include <atomic>
#include <cstdint>

namespace xpp {
namespace runtime {

/**
 * @brief Fixed-size single-producer multi-consumer ring buffer.
 *
 * @tparam T  Element type (pointer-sized).
 * @tparam N  Capacity. Must be a power of two.
 */
template <class T, size_t N = 256> class WorkStealingQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two");
  static constexpr uint32_t kMask = static_cast<uint32_t>(N - 1);

public:
  WorkStealingQueue() : m_head(0), m_tail(0) {}

  /**
   * @brief Push an item. Owner thread only.
   * @return false if full.
   */
  bool push(T item) {
    uint32_t h = m_head.load(std::memory_order_relaxed);
    uint32_t t = m_tail.load(std::memory_order_acquire);
    if (h - t >= N) return false;
    m_slots[h & kMask] = item;
    m_head.store(h + 1, std::memory_order_release);
    return true;
  }

  /**
   * @brief Pop an item. Owner thread only. LIFO order.
   * @return The item, or nullptr if empty.
   */
  T pop() {
    uint32_t h = m_head.load(std::memory_order_relaxed);
    uint32_t t = m_tail.load(std::memory_order_acquire);
    if (h == t) return nullptr; // empty
    --h;
    m_head.store(h, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    t = m_tail.load(std::memory_order_relaxed);
    if (t < h) {
      return m_slots[h & kMask]; // >1 element: index h uncontended
    }
    // Exactly one element (t == h): the owner's pop and a thief's steal
    // both target it, so claim it via the same tail CAS a stealer uses.
    if (t == h) {
      if (m_tail.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                         std::memory_order_relaxed)) {
        m_head.store(h + 1, std::memory_order_relaxed);
        return m_slots[h & kMask]; // we won
      }
    }
    // Lost the race — queue is empty.
    m_head.store(t, std::memory_order_relaxed);
    return nullptr;
  }

  /**
   * @brief Steal an item. Any thread. FIFO order (oldest first).
   * @return The item, or nullptr if empty or lost CAS race.
   */
  T steal() {
    uint32_t t = m_tail.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint32_t h = m_head.load(std::memory_order_acquire);
    if (t >= h) return nullptr;
    T item = m_slots[t & kMask];
    if (m_tail.compare_exchange_weak(t, t + 1, std::memory_order_seq_cst,
                                     std::memory_order_relaxed)) {
      return item;
    }
    return nullptr;
  }

  /**
   * @brief True if no items are in the queue.
   */
  bool empty() const {
    return m_tail.load(std::memory_order_acquire) >= m_head.load(std::memory_order_acquire);
  }

private:
  std::atomic<uint32_t> m_head; // owner writes, thieves read
  std::atomic<uint32_t> m_tail; // thieves CAS, owner reads
  T                     m_slots[N];
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_QUEUE_H
