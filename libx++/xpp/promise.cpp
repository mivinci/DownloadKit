/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise.cpp - Non-template Promise infrastructure:
 *   - WaitScope (thread_local registration)
 *   - Event::arm (xEventLoopPost bridge)
 *   - ChainPromiseNode::fire
 */

#include <xpp/promise.h>

extern "C" {
#include <x/base/event.h>
}

namespace xpp {

/* ── WaitScope ────────────────────────────────────────────────────── */

static thread_local xEventLoop t_current_loop = nullptr;

WaitScope::WaitScope(xEventLoop loop) : m_loop(loop) {
  XPP_ASSERT(loop != nullptr, "WaitScope: loop must not be null");
  XPP_ASSERT(t_current_loop == nullptr,
             "WaitScope: this thread already has an active WaitScope");
  t_current_loop = loop;
}

WaitScope::~WaitScope() {
  t_current_loop = nullptr;
}

xEventLoop WaitScope::current_loop() {
  return t_current_loop;
}

/* ── Event::arm ───────────────────────────────────────────────────── */

namespace _ {

static void event_post_callback(void *arg) {
  auto *event = static_cast<Event *>(arg);
  event->fire();
}

void Event::arm() {
  if (m_fired) return;
  xEventLoop loop = t_current_loop;
  if (loop) {
    xEventLoopPost(loop, event_post_callback, this);
  } else {
    // No loop — fire synchronously (for testing without a loop).
    fire();
  }
}

/* ── ChainPromiseNode::fire ───────────────────────────────────────── */

void ChainPromiseNode::fire() {
  XPP_ASSERT(m_state == Step1, "ChainPromiseNode::fire in wrong state");

  // Read the inner promise's node out of the dependency.
  // The dependency produced a Promise<T> as its "value". We need to
  // extract the OwnNode from that Promise and adopt it as our new inner.
  // Since read() gives us a type-erased void*, and the stored value is
  // a Promise<T>, we read it into a local and steal the node.
  //
  // Implementation note: the TransformNode before us stored the result
  // of func() which returned a Promise<T>. That Promise<T> is the value
  // we read here. We can't know T at this point (we're non-template),
  // so we read a raw OwnNode that was stored by a special mechanism.
  //
  // Actually, the design requires that when then() detects the func
  // returns Promise<U>, the TransformNode stores the OwnNode (not the
  // Promise wrapper) as the value. Let's define a PromiseBase that
  // ChainNode can read:

  // For simplicity, we do a direct OwnNode read. The TransformPromiseNode
  // for the chain case writes an OwnNode to dest (not a Promise<T>).
  // This is achieved by specializing TransformPromiseNode for when U is
  // a Promise<X> — but we chose the maybe_chain approach instead.
  //
  // With maybe_chain: TransformNode produces Promise<T> as U. Then
  // ChainNode wraps it. ChainNode's read of the TransformNode gives
  // Promise<T>. But read() takes void* — how do we extract the OwnNode?
  //
  // Solution: we read a PromiseNodeHolder which is just an OwnNode.
  // The TransformNode for the chain path stores its result as a
  // "value type" of Promise<X>, and when ChainNode reads it, it gets
  // the Promise<X> struct, from which it steals m_node.

  // Simplified: we'll read the dep into a buffer that IS a Promise<void>
  // (all Promise<T> have the same layout: just an OwnNode), steal the node.
  struct PromiseShell {
    OwnNode node;
  };
  PromiseShell shell;
  m_inner->read(&shell);

  // Adopt the inner node
  m_inner = std::move(shell.node);
  m_state = Step2;

  // Now poll the new inner node with the outer event
  if (m_outer_event) {
    m_inner->poll(m_outer_event);
  }

  m_fired = true;
}

} // namespace _
} // namespace xpp
