/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_pair.c - ICE candidate pair priority and sorting
 */

#include "ice_pair.h"

#include <stdlib.h>

uint64_t xIcePairPriority(uint32_t controlling_prio, uint32_t controlled_prio) {
  uint64_t g       = controlling_prio;
  uint64_t d       = controlled_prio;
  uint64_t min_val = (g < d) ? g : d;
  uint64_t max_val = (g > d) ? g : d;
  return ((uint64_t)1 << 32) * min_val + 2 * max_val + (g > d ? 1 : 0);
}

int xIcePairCompare(const void *a, const void *b) {
  const xIcePair *pa = (const xIcePair *)a;
  const xIcePair *pb = (const xIcePair *)b;
  /* Descending order: higher priority first */
  if (pa->priority > pb->priority) return -1;
  if (pa->priority < pb->priority) return 1;
  return 0;
}

void xIcePairSort(xIcePair *pairs, int count) {
  qsort(pairs, (size_t)count, sizeof(xIcePair), xIcePairCompare);
}
