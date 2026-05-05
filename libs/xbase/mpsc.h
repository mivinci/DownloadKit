/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc.h - Multi-producer single-consumer queue
 */

#ifndef XBASE_MPSC_H
#define XBASE_MPSC_H

#include <xbase/base.h>

/**
 * @brief Multi-producer single-consumer queue.
 * @ingroup xMpsc
 */
XDEF_STRUCT(xMpsc) {
  xMpsc *volatile next;
};

/**
 * @brief Push a node to the queue, can be called concurrently.
 * @ingroup xMpsc
 * @param head The head of the queue.
 * @param tail The tail of the queue.
 * @param node The node to push.
 */
XCAPI(void) xMpscPush(xMpsc **head, xMpsc **tail, xMpsc *node);

/**
 * @brief Pop a node from the queue, can only be called by one thread at a time.
 * @ingroup xMpsc
 * @param head The head of the queue.
 * @param tail The tail of the queue.
 * @return The node popped from the queue.
 */
XCAPI(xMpsc *) xMpscPop(xMpsc **head, xMpsc **tail);

/**
 * @brief Check if the queue is empty, can be called concurrently.
 * @ingroup xMpsc
 * @param head The head of the queue.
 * @return True if the queue is empty, false otherwise.
 */
XCAPI(bool) xMpscEmpty(xMpsc **head);

#endif // XBASE_MPSC_H
