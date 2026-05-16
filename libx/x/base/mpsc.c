/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.c - Multi-Producer Single-Consumer queue implementation
 */

#include <x/base/atomic.h>
#include <x/base/mpsc.h>

#include <stddef.h>

void xMpscPush(xMpsc **head, xMpsc **tail, xMpsc *node) {
  xMpsc *_tail;

  node->next = NULL;
  _tail      = xAtomicXchgPtr(tail, node, xAtomicAcqRel);

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
    /* queue has only one node,
     * try to update tail to NULL */
    xMpsc *expected = _head;
    if (!xAtomicCasPtrStrong(tail, &expected, NULL, xAtomicRelease)) {
      /* other thread is enqueueing new node, spin until it is done.
       * since only one thread at a time can pop nodes, it is safe to spin. */
      while (!_head_next) {
        _head_next = xAtomicLoad(&_head->next, xAtomicAcquire);
      }
      /* update head to the next node */
      xAtomicStore(head, _head_next, xAtomicRelease);
    } else {
      /* CAS succeeded: tail is now NULL.
       * Use CAS to set head to NULL only if head is still _head,
       * because a concurrent push may have already updated head. */
      expected = _head;
      xAtomicCasPtrStrong(head, &expected, NULL, xAtomicRelease);
    }
  } else {
    /* queue has more than one node, just advance head */
    xAtomicStore(head, _head_next, xAtomicRelease);
  }
  return _head;
}

bool xMpscEmpty(xMpsc **head) {
  return xAtomicLoad(head, xAtomicAcquire) == NULL;
}
