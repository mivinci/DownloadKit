/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_pair.h - ICE candidate pair representation and priority
 */

#ifndef XP2P_ICE_PAIR_H
#define XP2P_ICE_PAIR_H

#include "ice_candidate.h"
#include "ice_private.h"

/** Maximum number of candidate pairs.
 *  Increased to 1536 to accommodate birthday-attack spray pairs
 *  generated during symmetric NAT traversal. */
#define XICE_MAX_PAIRS 1536

/**
 * @brief An ICE candidate pair.
 */
XDEF_STRUCT(xIcePair) {
  xIceCandidate *local;     /**< Local candidate (not owned).   */
  xIceCandidate *remote;    /**< Remote candidate (not owned).  */
  uint64_t       priority;  /**< Pair priority.                 */
  xIcePairState  state;     /**< Pair state.                    */
  bool           nominated; /**< Whether this pair is nominated.*/
};

/**
 * @brief Compute candidate pair priority (RFC 8445 §6.1.2.3).
 *
 * pair_priority = 2^32 * MIN(G,D) + 2 * MAX(G,D) + (G>D ? 1 : 0)
 *
 * @param controlling_prio  Priority of the controlling agent's candidate.
 * @param controlled_prio   Priority of the controlled agent's candidate.
 * @return                  Pair priority.
 */
uint64_t xIcePairPriority(uint32_t controlling_prio, uint32_t controlled_prio);

/**
 * @brief Compare two candidate pairs by priority (for qsort).
 *
 * Higher priority comes first (descending order).
 */
int xIcePairCompare(const void *a, const void *b);

/**
 * @brief Sort an array of candidate pairs by priority (descending).
 */
void xIcePairSort(xIcePair *pairs, int count);

#endif /* XP2P_ICE_PAIR_H */
