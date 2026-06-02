/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * builder.h - Runtime configuration builder.
 *
 * Mirrors Tokio's runtime::Builder. Pick a flavor with
 * new_current_thread() / new_multi_thread(), tune it with the chained
 * setters, and produce a Runtime with build(). The plain Runtime
 * constructors are thin shortcuts over this builder.
 */

#ifndef XPP_RUNTIME_BUILDER_H
#define XPP_RUNTIME_BUILDER_H

#include <xpp/box.h>

#include <cstddef>
#include <string>
#include <utility>

namespace xpp {
namespace runtime {

class Runtime;

/* ── RuntimeFlavor ───────────────────────────────────────────────── */

enum class RuntimeFlavor {
  CurrentThread, // single-threaded: tasks run on the block_on thread
  MultiThread,   // work-stealing pool of worker threads
};

/* ── Builder ─────────────────────────────────────────────────────── */

/**
 * @brief Configures and constructs a Runtime.
 *
 *   auto rt = xpp::runtime::Builder::new_multi_thread()
 *               .worker_threads(4)
 *               .max_blocking_threads(256)
 *               .build();        // -> Box<Runtime>
 */
class Builder {
public:
  /** @brief A single-threaded runtime (tasks run on the block_on thread). */
  static Builder new_current_thread() {
    return Builder(RuntimeFlavor::CurrentThread);
  }
  /** @brief A multi-threaded, work-stealing runtime. */
  static Builder new_multi_thread() {
    return Builder(RuntimeFlavor::MultiThread);
  }

  /** @brief Worker thread count (MultiThread only). 0 = hardware concurrency. */
  Builder &worker_threads(size_t n) {
    m_worker_threads = n;
    return *this;
  }
  /** @brief Max concurrent threads for spawn_blocking work. */
  Builder &max_blocking_threads(size_t n) {
    m_max_blocking_threads = n;
    return *this;
  }
  /** @brief Cosmetic name hint for spawned threads (stored; advisory). */
  Builder &thread_name(std::string name) {
    m_thread_name = std::move(name);
    return *this;
  }

  // libx's xEventLoop always drives I/O readiness and timers, so these
  // tokio-parity toggles are accepted for API symmetry and are no-ops.
  Builder &enable_io() {
    return *this;
  }
  Builder &enable_time() {
    return *this;
  }
  Builder &enable_all() {
    return *this;
  }

  RuntimeFlavor flavor() const noexcept {
    return m_flavor;
  }

  /** @brief Construct the configured Runtime (heap-owned; Runtime is pinned). */
  Box<Runtime> build() const;

private:
  friend class Runtime;

  explicit Builder(RuntimeFlavor flavor)
      : m_flavor(flavor), m_worker_threads(0), m_max_blocking_threads(512) {}

  RuntimeFlavor m_flavor;
  size_t        m_worker_threads;       // 0 = auto (hardware concurrency)
  size_t        m_max_blocking_threads; // spawn_blocking thread cap
  std::string   m_thread_name;
};

} // namespace runtime
} // namespace xpp

#endif // XPP_RUNTIME_BUILDER_H
