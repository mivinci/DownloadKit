/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.c - Multi-Producer Single-Consumer queue implementation
 */

#include <xbase/atomic.h>
#include <xbase/mpsc.h>

#include <stddef.h>

void xMpscPush(xMpsc **head, xMpsc **tail, xMpsc *node) {
  xMpsc *_tail;

  node->next = NULL;
  _tail      = xAtomicXchg(tail, node, xAtomicAcqRel);

  if (_tail) {
    _tail->next = node;
  } else {
    /* update head if it is the first node */
    xAtomicStore(head, node, xAtomicRelease);
  }
}

xMpsc *xMpscPop(xMpsc **head, xMpsc **tail) {
  xMpsc *_head, *_head_next;

  _head = xAtomicLoad(head, xAtomicAcquire);
  if (!_head) {
    /* queue is empty */
    return NULL;
  }

  _head_next = xAtomicLoad(&_head->next, xAtomicAcquire);

  if (!_head_next) {
    /* queue is empty,
     * try to update tail to NULL */
    if (!xAtomicCasStrong(tail, &_head, NULL, xAtomicRelease)) {
      /* other thread is enqueueing new node，spin until it is done.
       * since only thread at a time can pop nodes, it is safe to spin */
      while (!_head_next) {
        _head_next = xAtomicLoad(&_head->next, xAtomicAcquire);
      }
    }
  }

  /* update head */
  xAtomicStore(head, _head_next, xAtomicRelaxed);
  return _head;
}

bool xMpscEmpty(xMpsc **head) {
  return xAtomicLoad(head, xAtomicAcquire) == NULL;
}
