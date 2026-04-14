/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_candidate_test.cpp - Unit tests for ICE candidate and pair
 */

#include <gtest/gtest.h>

extern "C" {
#include "ice_candidate.h"
#include "ice_pair.h"
}

#include <arpa/inet.h>

/* ───────────────────── Candidate Priority ───────────────────── */

TEST(IceCandidateTest, HostPriorityFormula) {
  /* host: type_pref=126, local_pref=65535, component=1 */
  uint32_t prio = xIceCandidatePriority(xIceCandidateType_Host, 65535, 1);
  /* (126 << 24) | (65535 << 8) | 255 = 2130706431 */
  EXPECT_EQ(prio, 2130706431u);
}

TEST(IceCandidateTest, SrflxPriorityFormula) {
  /* srflx: type_pref=100, local_pref=65535, component=1 */
  uint32_t prio = xIceCandidatePriority(xIceCandidateType_Srflx, 65535, 1);
  /* (100 << 24) | (65535 << 8) | 255 = 1694498815 */
  EXPECT_EQ(prio, 1694498815u);
}

TEST(IceCandidateTest, RelayPriorityFormula) {
  /* relay: type_pref=5, local_pref=65535, component=1 */
  uint32_t prio = xIceCandidatePriority(xIceCandidateType_Relay, 65535, 1);
  /* (5 << 24) | (65535 << 8) | 255 = 100663295 */
  EXPECT_EQ(prio, 100663295u);
}

TEST(IceCandidateTest, PriorityOrdering) {
  uint32_t host  = xIceCandidatePriority(xIceCandidateType_Host, 65535, 1);
  uint32_t srflx = xIceCandidatePriority(xIceCandidateType_Srflx, 65535, 1);
  uint32_t prflx = xIceCandidatePriority(xIceCandidateType_Prflx, 65535, 1);
  uint32_t relay = xIceCandidatePriority(xIceCandidateType_Relay, 65535, 1);

  EXPECT_GT(host, prflx);
  EXPECT_GT(prflx, srflx);
  EXPECT_GT(srflx, relay);
}

TEST(IceCandidateTest, ComponentIdAffectsPriority) {
  uint32_t comp1 = xIceCandidatePriority(xIceCandidateType_Host, 65535, 1);
  uint32_t comp2 = xIceCandidatePriority(xIceCandidateType_Host, 65535, 2);
  EXPECT_GT(comp1, comp2);
}

/* ───────────────────── Foundation ───────────────────── */

TEST(IceCandidateTest, SameTypeAndBaseGiveSameFoundation) {
  xIceCandidate c1, c2;
  memset(&c1, 0, sizeof(c1));
  memset(&c2, 0, sizeof(c2));

  c1.type = xIceCandidateType_Host;
  c2.type = xIceCandidateType_Host;

  struct sockaddr_in *a1 = (struct sockaddr_in *)&c1.base_addr;
  a1->sin_family         = AF_INET;
  inet_pton(AF_INET, "192.168.1.1", &a1->sin_addr);

  struct sockaddr_in *a2 = (struct sockaddr_in *)&c2.base_addr;
  a2->sin_family         = AF_INET;
  inet_pton(AF_INET, "192.168.1.1", &a2->sin_addr);

  xIceCandidateFoundation(&c1, NULL);
  xIceCandidateFoundation(&c2, NULL);

  EXPECT_STREQ(c1.foundation, c2.foundation);
}

TEST(IceCandidateTest, DifferentTypeGivesDifferentFoundation) {
  xIceCandidate c1, c2;
  memset(&c1, 0, sizeof(c1));
  memset(&c2, 0, sizeof(c2));

  c1.type = xIceCandidateType_Host;
  c2.type = xIceCandidateType_Srflx;

  struct sockaddr_in *a1 = (struct sockaddr_in *)&c1.base_addr;
  a1->sin_family         = AF_INET;
  inet_pton(AF_INET, "192.168.1.1", &a1->sin_addr);

  struct sockaddr_in *a2 = (struct sockaddr_in *)&c2.base_addr;
  a2->sin_family         = AF_INET;
  inet_pton(AF_INET, "192.168.1.1", &a2->sin_addr);

  xIceCandidateFoundation(&c1, NULL);
  xIceCandidateFoundation(&c2, NULL);

  EXPECT_STRNE(c1.foundation, c2.foundation);
}

/* ───────────────────── Type String ───────────────────── */

TEST(IceCandidateTest, TypeStringRoundTrip) {
  xIceCandidateType types[] = {
    xIceCandidateType_Host,
    xIceCandidateType_Srflx,
    xIceCandidateType_Prflx,
    xIceCandidateType_Relay,
  };

  for (auto t : types) {
    const char       *str = xIceCandidateTypeStr(t);
    xIceCandidateType parsed;
    ASSERT_EQ(xIceCandidateTypeFromStr(str, &parsed), xErrno_Ok);
    EXPECT_EQ(parsed, t);
  }
}

TEST(IceCandidateTest, InvalidTypeString) {
  xIceCandidateType t;
  EXPECT_NE(xIceCandidateTypeFromStr("invalid", &t), xErrno_Ok);
}

/* ───────────────────── Sockaddr Helpers ───────────────────── */

TEST(IceCandidateTest, SockaddrPortIPv4) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(12345);
  EXPECT_EQ(xSockaddrPort((struct sockaddr *)&addr), 12345);
}

TEST(IceCandidateTest, SockaddrIPv4String) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "10.0.0.1", &addr.sin_addr);

  char        buf[64];
  const char *result = xSockaddrIP((struct sockaddr *)&addr, buf, sizeof(buf));
  ASSERT_NE(result, nullptr);
  EXPECT_STREQ(result, "10.0.0.1");
}

/* ───────────────────── Candidate Pair Priority ───────────────────── */

TEST(IcePairTest, PairPriorityFormula) {
  /* G=100, D=50 → 2^32 * 50 + 2 * 100 + 1 = 214748364901 */
  uint64_t prio = xIcePairPriority(100, 50);
  EXPECT_EQ(prio, (uint64_t)50 * ((uint64_t)1 << 32) + 200 + 1);
}

TEST(IcePairTest, PairPrioritySymmetry) {
  /* When G < D, the formula gives a different result */
  uint64_t p1 = xIcePairPriority(100, 200);
  uint64_t p2 = xIcePairPriority(200, 100);
  EXPECT_NE(p1, p2);
}

TEST(IcePairTest, PairPriorityEqual) {
  /* When G == D, (G>D ? 1 : 0) = 0 */
  uint64_t prio = xIcePairPriority(100, 100);
  EXPECT_EQ(prio, (uint64_t)100 * ((uint64_t)1 << 32) + 200 + 0);
}

/* ───────────────────── Pair Sorting ───────────────────── */

TEST(IcePairTest, SortDescending) {
  xIceCandidate locals[3], remotes[3];
  memset(locals, 0, sizeof(locals));
  memset(remotes, 0, sizeof(remotes));

  xIcePair pairs[3];
  pairs[0].local     = &locals[0];
  pairs[0].remote    = &remotes[0];
  pairs[0].priority  = 100;
  pairs[0].state     = xIcePairState_Frozen;
  pairs[0].nominated = false;

  pairs[1].local     = &locals[1];
  pairs[1].remote    = &remotes[1];
  pairs[1].priority  = 300;
  pairs[1].state     = xIcePairState_Frozen;
  pairs[1].nominated = false;

  pairs[2].local     = &locals[2];
  pairs[2].remote    = &remotes[2];
  pairs[2].priority  = 200;
  pairs[2].state     = xIcePairState_Frozen;
  pairs[2].nominated = false;

  xIcePairSort(pairs, 3);

  EXPECT_EQ(pairs[0].priority, 300u);
  EXPECT_EQ(pairs[1].priority, 200u);
  EXPECT_EQ(pairs[2].priority, 100u);
}
