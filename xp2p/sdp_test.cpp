/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sdp_test.cpp - Unit tests for SDP encoding / decoding
 */

#include <gtest/gtest.h>

extern "C" {
#include "sdp.h"
}

#include <arpa/inet.h>
#include <string.h>

/* ───────────────────── Candidate Line ───────────────────── */

TEST(SdpTest, EncodeCandidateHost) {
  xIceCandidate cand;
  memset(&cand, 0, sizeof(cand));
  strncpy(cand.foundation, "12345", XICE_FOUNDATION_MAX_LEN);
  cand.component_id = 1;
  cand.priority     = 2130706431;
  cand.type         = xIceCandidateType_Host;

  struct sockaddr_in *a4 = (struct sockaddr_in *)&cand.addr;
  a4->sin_family         = AF_INET;
  a4->sin_port           = htons(5000);
  inet_pton(AF_INET, "192.168.1.100", &a4->sin_addr);

  char buf[256];
  int  len = xIceSdpEncodeCandidate(&cand, buf, sizeof(buf));
  ASSERT_GT(len, 0);

  EXPECT_NE(strstr(buf, "a=candidate:12345"), nullptr);
  EXPECT_NE(strstr(buf, "192.168.1.100"), nullptr);
  EXPECT_NE(strstr(buf, "5000"), nullptr);
  EXPECT_NE(strstr(buf, "typ host"), nullptr);
}

TEST(SdpTest, EncodeCandidateSrflx) {
  xIceCandidate cand;
  memset(&cand, 0, sizeof(cand));
  strncpy(cand.foundation, "99999", XICE_FOUNDATION_MAX_LEN);
  cand.component_id = 1;
  cand.priority     = 1694498815;
  cand.type         = xIceCandidateType_Srflx;

  struct sockaddr_in *a4 = (struct sockaddr_in *)&cand.addr;
  a4->sin_family         = AF_INET;
  a4->sin_port           = htons(3000);
  inet_pton(AF_INET, "203.0.113.5", &a4->sin_addr);

  struct sockaddr_in *r4 = (struct sockaddr_in *)&cand.rel_addr;
  r4->sin_family         = AF_INET;
  r4->sin_port           = htons(5000);
  inet_pton(AF_INET, "192.168.1.100", &r4->sin_addr);

  char buf[256];
  int  len = xIceSdpEncodeCandidate(&cand, buf, sizeof(buf));
  ASSERT_GT(len, 0);

  EXPECT_NE(strstr(buf, "typ srflx"), nullptr);
  EXPECT_NE(strstr(buf, "raddr 192.168.1.100"), nullptr);
  EXPECT_NE(strstr(buf, "rport 5000"), nullptr);
}

TEST(SdpTest, DecodeCandidateHost) {
  const char *line =
    "a=candidate:12345 1 UDP 2130706431 192.168.1.100 5000 typ host";
  xIceCandidate cand;
  ASSERT_EQ(xIceSdpDecodeCandidate(line, &cand), xErrno_Ok);

  EXPECT_STREQ(cand.foundation, "12345");
  EXPECT_EQ(cand.component_id, 1);
  EXPECT_EQ(cand.priority, 2130706431u);
  EXPECT_EQ(cand.type, xIceCandidateType_Host);

  struct sockaddr_in *a4 = (struct sockaddr_in *)&cand.addr;
  EXPECT_EQ(a4->sin_family, AF_INET);
  EXPECT_EQ(ntohs(a4->sin_port), 5000);

  char ip[32];
  inet_ntop(AF_INET, &a4->sin_addr, ip, sizeof(ip));
  EXPECT_STREQ(ip, "192.168.1.100");
}

TEST(SdpTest, DecodeCandidateSrflx) {
  const char *line = "a=candidate:99999 1 UDP 1694498815 203.0.113.5 3000 typ "
                     "srflx raddr 192.168.1.100 rport 5000";
  xIceCandidate cand;
  ASSERT_EQ(xIceSdpDecodeCandidate(line, &cand), xErrno_Ok);

  EXPECT_EQ(cand.type, xIceCandidateType_Srflx);

  struct sockaddr_in *r4 = (struct sockaddr_in *)&cand.rel_addr;
  EXPECT_EQ(r4->sin_family, AF_INET);
  EXPECT_EQ(ntohs(r4->sin_port), 5000);

  char ip[32];
  inet_ntop(AF_INET, &r4->sin_addr, ip, sizeof(ip));
  EXPECT_STREQ(ip, "192.168.1.100");
}

TEST(SdpTest, CandidateRoundTrip) {
  xIceCandidate orig;
  memset(&orig, 0, sizeof(orig));
  strncpy(orig.foundation, "42", XICE_FOUNDATION_MAX_LEN);
  orig.component_id = 1;
  orig.priority     = 100;
  orig.type         = xIceCandidateType_Host;

  struct sockaddr_in *a4 = (struct sockaddr_in *)&orig.addr;
  a4->sin_family         = AF_INET;
  a4->sin_port           = htons(8080);
  inet_pton(AF_INET, "10.0.0.1", &a4->sin_addr);

  char buf[256];
  int  len = xIceSdpEncodeCandidate(&orig, buf, sizeof(buf));
  ASSERT_GT(len, 0);

  xIceCandidate decoded;
  ASSERT_EQ(xIceSdpDecodeCandidate(buf, &decoded), xErrno_Ok);

  EXPECT_STREQ(decoded.foundation, orig.foundation);
  EXPECT_EQ(decoded.component_id, orig.component_id);
  EXPECT_EQ(decoded.priority, orig.priority);
  EXPECT_EQ(decoded.type, orig.type);
}

/* ───────────────────── Full SDP ───────────────────── */

TEST(SdpTest, FullSdpEncodeDecodeRoundTrip) {
  xIceCandidate cands[2];
  memset(cands, 0, sizeof(cands));

  strncpy(cands[0].foundation, "1", XICE_FOUNDATION_MAX_LEN);
  cands[0].component_id  = 1;
  cands[0].priority      = 2130706431;
  cands[0].type          = xIceCandidateType_Host;
  struct sockaddr_in *a0 = (struct sockaddr_in *)&cands[0].addr;
  a0->sin_family         = AF_INET;
  a0->sin_port           = htons(5000);
  inet_pton(AF_INET, "192.168.1.1", &a0->sin_addr);

  strncpy(cands[1].foundation, "2", XICE_FOUNDATION_MAX_LEN);
  cands[1].component_id  = 1;
  cands[1].priority      = 1694498815;
  cands[1].type          = xIceCandidateType_Srflx;
  struct sockaddr_in *a1 = (struct sockaddr_in *)&cands[1].addr;
  a1->sin_family         = AF_INET;
  a1->sin_port           = htons(3000);
  inet_pton(AF_INET, "203.0.113.5", &a1->sin_addr);
  struct sockaddr_in *r1 = (struct sockaddr_in *)&cands[1].rel_addr;
  r1->sin_family         = AF_INET;
  r1->sin_port           = htons(5000);
  inet_pton(AF_INET, "192.168.1.1", &r1->sin_addr);

  char sdp[XSDP_MAX_SIZE];
  int  len = xIceSdpEncode("myufrag", "mysupersecretpassword22", cands, 2, true,
                           sdp, sizeof(sdp));
  ASSERT_GT(len, 0);

  xIceSdp parsed;
  ASSERT_EQ(xIceSdpDecode(sdp, len, &parsed), xErrno_Ok);

  EXPECT_STREQ(parsed.ice_ufrag, "myufrag");
  EXPECT_STREQ(parsed.ice_pwd, "mysupersecretpassword22");
  EXPECT_TRUE(parsed.trickle);
  EXPECT_FALSE(parsed.end_of_candidates);
  EXPECT_EQ(parsed.candidate_count, 2);

  EXPECT_STREQ(parsed.candidates[0].foundation, "1");
  EXPECT_EQ(parsed.candidates[0].type, xIceCandidateType_Host);
  EXPECT_STREQ(parsed.candidates[1].foundation, "2");
  EXPECT_EQ(parsed.candidates[1].type, xIceCandidateType_Srflx);
}

TEST(SdpTest, DecodeMissingUfragFails) {
  const char *sdp = "v=0\r\na=ice-pwd:password\r\n";
  xIceSdp     parsed;
  EXPECT_NE(xIceSdpDecode(sdp, strlen(sdp), &parsed), xErrno_Ok);
}

TEST(SdpTest, DecodeMissingPwdFails) {
  const char *sdp = "v=0\r\na=ice-ufrag:ufrag\r\n";
  xIceSdp     parsed;
  EXPECT_NE(xIceSdpDecode(sdp, strlen(sdp), &parsed), xErrno_Ok);
}

TEST(SdpTest, DecodeEndOfCandidates) {
  const char *sdp = "v=0\r\n"
                    "a=ice-ufrag:test\r\n"
                    "a=ice-pwd:testpassword1234567890\r\n"
                    "a=end-of-candidates\r\n";
  xIceSdp     parsed;
  ASSERT_EQ(xIceSdpDecode(sdp, strlen(sdp), &parsed), xErrno_Ok);
  EXPECT_TRUE(parsed.end_of_candidates);
}

TEST(SdpTest, DecodeInvalidCandidateLineSkipped) {
  const char *sdp = "v=0\r\n"
                    "a=ice-ufrag:test\r\n"
                    "a=ice-pwd:testpassword1234567890\r\n"
                    "a=candidate:garbage line\r\n"
                    "a=candidate:1 1 UDP 100 10.0.0.1 5000 typ host\r\n";
  xIceSdp     parsed;
  ASSERT_EQ(xIceSdpDecode(sdp, strlen(sdp), &parsed), xErrno_Ok);
  /* Only the valid candidate should be parsed */
  EXPECT_EQ(parsed.candidate_count, 1);
}
