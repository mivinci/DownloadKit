/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * list.h - Doubly-linked circular list (inline implementation)
 *
 * Derived from the Linux kernel's include/linux/list.h.
 * Original design by (C) Linus Torvalds and other Linux kernel
 * contributors, licensed under GPL-2.0-only.  The moo adaptation
 * re-implements the same intrusive list idea under the MIT licence
 * granted above; no Linux kernel source code is included verbatim.
 */

#ifndef XBASE_LIST_H
#define XBASE_LIST_H

#include <x/base/base.h>

/**
 * @brief Doubly-linked list node.
 * @ingroup xList
 *
 * Embed this into a struct, then use xContainerOf to get the struct back.
 */
XDEF_STRUCT(xList) {
  xList *next;
  xList *prev;
};

/**
 * @brief Initialize a list head (makes it a circular empty list).
 * @ingroup xList
 * @param head The list head to initialize.
 */
XCAPI_INLINE(void) xListInit(xList *head) {
  head->next = head;
  head->prev = head;
}

/**
 * @brief Add a node after a given node.
 * @ingroup xList
 * @param prev The node after which to insert.
 * @param node The node to insert.
 */
XCAPI_INLINE(void) xListAdd(xList *prev, xList *node) {
  xList *next = prev->next;

  next->prev = node;
  node->next = next;
  node->prev = prev;
  prev->next = node;
}

/**
 * @brief Add a node at list tail (equivalent to xListAddBefore(head, node)).
 * @ingroup xList
 * @param head The list head.
 * @param node The node to insert.
 */
XCAPI_INLINE(void) xListAddTail(xList *head, xList *node) {
  xListAdd(head->prev, node);
}

/**
 * @brief Add a node at list head (equivalent to xListAdd(head, node)).
 * @ingroup xList
 * @param head The list head.
 * @param node The node to insert.
 */
XCAPI_INLINE(void) xListAddHead(xList *head, xList *node) {
  xListAdd(head, node);
}

/**
 * @brief Add a node before a given node.
 * @ingroup xList
 * @param next The node before which to insert.
 * @param node The node to insert.
 */
XCAPI_INLINE(void) xListAddBefore(xList *next, xList *node) {
  xList *prev = next->prev;

  next->prev = node;
  node->next = next;
  node->prev = prev;
  prev->next = node;
}

/**
 * @brief Remove a node from its list.
 * @ingroup xList
 * @param node The node to remove.
 */
XCAPI_INLINE(void) xListDel(xList *node) {
  xList *next = node->next;
  xList *prev = node->prev;

  next->prev = prev;
  prev->next = next;

  /* poison for safety debugging */
  node->next = (xList *)0xDEAD;
  node->prev = (xList *)0xBEEF;
}

/**
 * @brief Check if a list is empty.
 * @ingroup xList
 * @param head The list head to check.
 * @return True if the list is empty, false otherwise.
 */
XCAPI_INLINE(bool) xListEmpty(xList *head) {
  return head->next == head;
}

/**
 * @brief Macro to iterate over a list.
 * @ingroup xList
 * @param pos  The pointer to use as the iterator (xList *).
 * @param head The head of the list.
 */
#define xListForEach(pos, head) \
  for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/**
 * @brief Macro to iterate over a list safely (allows deletion during
 * iteration).
 * @ingroup xList
 * @param pos  The pointer to use as the iterator (xList *).
 * @param tmp  A temporary pointer for safe iteration (xList *).
 * @param head The head of the list.
 */
#define xListForEachSafe(pos, tmp, head)                           \
  for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head); \
       (pos) = (tmp), (tmp) = (pos)->next)

/**
 * @brief Macro to iterate over list entries.
 * @ingroup xList
 * @param pos    The pointer to use as the iterator (struct pointer).
 * @param head   The head of the list (xList *).
 * @param member The name of the xList member inside the struct.
 */
/* typeof is GCC-specific; use std::remove_reference for MSVC C++ compatibility */
#ifdef _MSC_VER
#include <type_traits>
#define xListTypeof(expr) typename std::remove_reference<decltype(expr)>::type
#else
#define xListTypeof(expr) typeof(expr)
#endif

#define xListForEachEntry(pos, head, member)                            \
  for ((pos) = xContainerOf((head)->next, xListTypeof(*(pos)), member); \
       &(pos)->member != (head);                                        \
       (pos) = xContainerOf((pos)->member.next, xListTypeof(*(pos)), member))

/**
 * @brief Macro to iterate over list entries safely.
 * @ingroup xList
 * @param pos    The pointer to use as the iterator (struct pointer).
 * @param tmp    A temporary pointer for safe iteration (struct pointer).
 * @param head   The head of the list (xList *).
 * @param member The name of the xList member inside the struct.
 */
#define xListForEachEntrySafe(pos, tmp, head, member)                         \
  for ((pos) = xContainerOf((head)->next, xListTypeof(*(pos)), member),       \
      (tmp)  = xContainerOf((pos)->member.next, xListTypeof(*(tmp)), member); \
       &(pos)->member != (head); (pos) = (tmp),                              \
      (tmp) = xContainerOf((pos)->member.next, xListTypeof(*(tmp)), member))

#endif // XBASE_LIST_H
