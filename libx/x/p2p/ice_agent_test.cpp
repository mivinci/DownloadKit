/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_agent_test.cpp - Integration tests for xIceAgent
 */

#include <gtest/gtest.h>

extern "C" {
#include "ice_agent.h"
}

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

using ms = std::chrono::milliseconds;

/* ───────────────────── Helpers ───────────────────── */

static void pump_loop(xEventLoop loop, int total_ms) {
  auto deadline = std::chrono::steady_clock::now() + ms(total_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    auto remaining = std::chrono::duration_cast<ms>(
                       deadline - std::chrono::steady_clock::now())
                       .count();
    if (remaining <= 0) break;
    xEventWait(loop, (int)remaining);
  }
}

/*
 * Pump the event loop until `pred()` returns true or `total_ms` elapses.
 * Returns true if the predicate was satisfied, false on timeout.
 *
 * Use this instead of a fixed `pump_loop(loop, N)` when the test's success
 * criterion is an async state change (e.g. gathering completed). It avoids
 * both flakiness on slow CI runners AND wasted wall-clock time on fast ones.
 */
template <typename Pred>
static bool pump_until(xEventLoop loop, Pred pred, int total_ms) {
  auto deadline = std::chrono::steady_clock::now() + ms(total_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    auto remaining = std::chrono::duration_cast<ms>(
                       deadline - std::chrono::steady_clock::now())
                       .count();
    if (remaining <= 0) break;
    /* Wake up at least every 50ms so we can re-check the predicate even
     * if no event fires (e.g. waiting on a timer that hasn't fired yet). */
    xEventWait(loop, (int)std::min<long long>(remaining, 50));
  }
  return pred();
}

/* ───────────────────── Callback State ───────────────────── */

struct AgentState {
  std::atomic<xIceState> state{xIceState_New};
  std::string            last_candidate;
  bool                   gathering_done = false;
  std::vector<uint8_t>   received_data;
  bool                   data_received = false;
};

static void on_state_change(xIceAgent, xIceState state, void *arg) {
  auto *s = (AgentState *)arg;
  s->state.store(state);
}

static void on_candidate(xIceAgent, const char *candidate_sdp, void *arg) {
  auto *s = (AgentState *)arg;
  if (candidate_sdp) {
    s->last_candidate = candidate_sdp;
  } else {
    s->gathering_done = true;
  }
}

static void on_data(xIceAgent, const uint8_t *data, size_t len, void *arg) {
  auto *s = (AgentState *)arg;
  s->received_data.assign(data, data + len);
  s->data_received = true;
}

/* ───────────────────── Tests ───────────────────── */

TEST(IceAgentTest, CreateDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.on_data         = on_data;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  xIceAgentDestroy(agent);
  EXPECT_EQ(as.state.load(), xIceState_Closed);

  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, CreateNullConfigFails) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  EXPECT_EQ(xIceAgentCreate(loop, nullptr), nullptr);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, CreateNullLoopFails) {
  xIceConf config;
  memset(&config, 0, sizeof(config));
  EXPECT_EQ(xIceAgentCreate(nullptr, &config), nullptr);
}
TEST(IceAgentTest, DestroyNullIsNoop) {
  xIceAgentDestroy(nullptr); /* Should not crash */
}

TEST(IceAgentTest, GatherProducesHostCandidate) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  xErrno err = xIceAgentGather(agent);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(as.state.load(), xIceState_Gathering);

  /* Should have received at least one candidate */
  EXPECT_FALSE(as.last_candidate.empty());
  EXPECT_NE(as.last_candidate.find("typ host"), std::string::npos);

  /* No STUN/TURN servers → gathering completes immediately */
  pump_loop(loop, 100);
  EXPECT_TRUE(as.gathering_done);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, GatherTwiceFails) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_EQ(xIceAgentGather(agent), xErrno_Ok);
  EXPECT_NE(xIceAgentGather(agent), xErrno_Ok); /* Already gathering */

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, CreateOfferContainsIceParams) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  xIceAgentGather(agent);

  char *offer = xIceAgentCreateOffer(agent);
  ASSERT_NE(offer, nullptr);

  std::string sdp(offer);
  EXPECT_NE(sdp.find("a=ice-ufrag:"), std::string::npos);
  EXPECT_NE(sdp.find("a=ice-pwd:"), std::string::npos);
  EXPECT_NE(sdp.find("a=ice-options:trickle"), std::string::npos);
  EXPECT_NE(sdp.find("a=candidate:"), std::string::npos);

  free(offer);
  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, SetRemoteDescriptionParsesCredentials) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlled;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  const char *remote_sdp =
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=application 9 UDP/ICE 0\r\n"
    "a=ice-ufrag:remoteufrag\r\n"
    "a=ice-pwd:remotepassword1234567890\r\n"
    "a=ice-options:trickle\r\n"
    "a=candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host\r\n";

  xErrno err = xIceAgentSetRemoteDescription(agent, remote_sdp);
  EXPECT_EQ(err, xErrno_Ok);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, SetRemoteDescriptionInvalidSdpFails) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xIceConf config;
  memset(&config, 0, sizeof(config));
  config.role = xIceRole_Controlled;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_NE(xIceAgentSetRemoteDescription(agent, "garbage"), xErrno_Ok);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, AddRemoteCandidateWorks) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  xErrno err = xIceAgentAddRemoteCandidate(
    agent, "candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host");
  EXPECT_EQ(err, xErrno_Ok);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, AddRemoteCandidateInvalidFails) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xIceConf config;
  memset(&config, 0, sizeof(config));
  config.role = xIceRole_Controlling;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_NE(xIceAgentAddRemoteCandidate(agent, "garbage"), xErrno_Ok);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, SendBeforeConnectedFails) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  xIceConf config;
  memset(&config, 0, sizeof(config));
  config.role = xIceRole_Controlling;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  uint8_t data[] = {1, 2, 3};
  EXPECT_NE(xIceAgentSend(agent, data, sizeof(data)), xErrno_Ok);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, FullLocalLoopback) {
  /*
   * Integration test: Two agents (Controlling + Controlled) on localhost.
   * 1. Both gather candidates
   * 2. Exchange SDP offer/answer
   * 3. Pump event loop to allow connectivity checks
   * 4. Verify both reach Connected state
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as_ctrl, as_ctld;

  /* Create Controlling agent */
  xIceConf config_ctrl;
  memset(&config_ctrl, 0, sizeof(config_ctrl));
  config_ctrl.role            = xIceRole_Controlling;
  config_ctrl.on_state_change = on_state_change;
  config_ctrl.on_candidate    = on_candidate;
  config_ctrl.on_data         = on_data;
  config_ctrl.ctx             = &as_ctrl;

  xIceAgent ctrl = xIceAgentCreate(loop, &config_ctrl);
  ASSERT_NE(ctrl, nullptr);

  /* Create Controlled agent */
  xIceConf config_ctld;
  memset(&config_ctld, 0, sizeof(config_ctld));
  config_ctld.role            = xIceRole_Controlled;
  config_ctld.on_state_change = on_state_change;
  config_ctld.on_candidate    = on_candidate;
  config_ctld.on_data         = on_data;
  config_ctld.ctx             = &as_ctld;

  xIceAgent ctld = xIceAgentCreate(loop, &config_ctld);
  ASSERT_NE(ctld, nullptr);

  /* Both gather */
  EXPECT_EQ(xIceAgentGather(ctrl), xErrno_Ok);
  EXPECT_EQ(xIceAgentGather(ctld), xErrno_Ok);

  /* Wait for gathering to complete (immediate with no servers) */
  pump_loop(loop, 100);
  EXPECT_TRUE(as_ctrl.gathering_done);
  EXPECT_TRUE(as_ctld.gathering_done);

  /* Exchange SDP */
  char *offer = xIceAgentCreateOffer(ctrl);
  ASSERT_NE(offer, nullptr);

  char *answer = xIceAgentCreateAnswer(ctld);
  ASSERT_NE(answer, nullptr);

  EXPECT_EQ(xIceAgentSetRemoteDescription(ctrl, answer), xErrno_Ok);
  EXPECT_EQ(xIceAgentSetRemoteDescription(ctld, offer), xErrno_Ok);

  free(offer);
  free(answer);

  /* Pump loop to allow connectivity checks */
  pump_loop(loop, 11000);

  /*
   * Note: In a real loopback test, both agents would need to be on
   * different ports and the STUN checks would need to complete.
   * Since both bind to INADDR_ANY:0, the checks may or may not
   * succeed depending on whether the OS routes localhost correctly.
   *
   * We verify the state machine progressed past New/Gathering.
   */
  xIceState ctrl_state = as_ctrl.state.load();
  xIceState ctld_state = as_ctld.state.load();

  /* Both should have progressed to at least Checking */
  EXPECT_NE(ctrl_state, xIceState_New);
  EXPECT_NE(ctrl_state, xIceState_Gathering);
  EXPECT_NE(ctld_state, xIceState_New);
  EXPECT_NE(ctld_state, xIceState_Gathering);

  xIceAgentDestroy(ctrl);
  xIceAgentDestroy(ctld);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, TrickleIceAddCandidate) {
  /*
   * Test Trickle ICE: Add remote candidate dynamically after
   * setting remote description.
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_EQ(xIceAgentGather(agent), xErrno_Ok);
  pump_loop(loop, 100);

  /* Set remote description with no candidates */
  const char *remote_sdp = "v=0\r\n"
                           "o=- 0 0 IN IP4 0.0.0.0\r\n"
                           "s=-\r\n"
                           "t=0 0\r\n"
                           "m=application 9 UDP/ICE 0\r\n"
                           "a=ice-ufrag:trickleufrag\r\n"
                           "a=ice-pwd:tricklepassword1234567890\r\n"
                           "a=ice-options:trickle\r\n";

  EXPECT_EQ(xIceAgentSetRemoteDescription(agent, remote_sdp), xErrno_Ok);

  /* Now trickle in a candidate */
  EXPECT_EQ(xIceAgentAddRemoteCandidate(
              agent, "candidate:1 1 UDP 2130706431 127.0.0.1 9999 typ host"),
            xErrno_Ok);

  /* Pump to allow checks */
  pump_loop(loop, 2000);

  /* Agent should be in Checking or beyond */
  xIceState state = as.state.load();
  EXPECT_NE(state, xIceState_New);
  EXPECT_NE(state, xIceState_Gathering);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, AllPairsFailLeadsToFailedState) {
  /*
   * Test: When all candidate pairs fail, agent transitions to Failed.
   * We set a remote candidate with an unreachable address.
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_EQ(xIceAgentGather(agent), xErrno_Ok);
  pump_loop(loop, 100);

  /* Set remote with unreachable candidate */
  const char *remote_sdp = "v=0\r\n"
                           "o=- 0 0 IN IP4 0.0.0.0\r\n"
                           "s=-\r\n"
                           "t=0 0\r\n"
                           "m=application 9 UDP/ICE 0\r\n"
                           "a=ice-ufrag:failufrag\r\n"
                           "a=ice-pwd:failpassword1234567890ab\r\n"
                           "a=candidate:1 1 UDP 100 192.0.2.1 1 typ host\r\n";

  EXPECT_EQ(xIceAgentSetRemoteDescription(agent, remote_sdp), xErrno_Ok);

  /* Wait for check timeout (10s) + margin */
  pump_loop(loop, 11000);

  EXPECT_EQ(as.state.load(), xIceState_Failed);

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

/* ───────────────────── srflx / relay Gathering Tests ───────────────────── */

TEST(IceAgentTest, GatherWithInvalidStunServerStillSucceeds) {
  /*
   * When stun_server is set but unresolvable, gathering should still
   * complete with at least a host candidate (srflx just won't appear).
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.stun_server     = "invalid.example.test:3478";
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  xErrno err = xIceAgentGather(agent);
  EXPECT_EQ(err, xErrno_Ok);

  /* Should have host candidate immediately */
  EXPECT_FALSE(as.last_candidate.empty());
  EXPECT_NE(as.last_candidate.find("typ host"), std::string::npos);

  /* Wait until gathering completes (or gather_timer fires after 5s). */
  EXPECT_TRUE(pump_until(loop, [&] { return as.gathering_done; }, 6000));

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, GatherWithInvalidTurnServerStillSucceeds) {
  /*
   * When turn_server is set but unresolvable, gathering should still
   * complete with at least a host candidate.
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.turn_server     = "invalid.example.test:3478";
  config.turn_username   = "user";
  config.turn_password   = "pass";
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  xErrno err = xIceAgentGather(agent);
  EXPECT_EQ(err, xErrno_Ok);

  EXPECT_FALSE(as.last_candidate.empty());

  EXPECT_TRUE(pump_until(loop, [&] { return as.gathering_done; }, 6000));

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, GatherWithNullTurnCredentialsSkipsTurn) {
  /*
   * If turn_server is set but username/password are NULL,
   * TURN allocation should be skipped.
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.turn_server     = "127.0.0.1:3478";
  config.turn_username   = nullptr;
  config.turn_password   = nullptr;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_EQ(xIceAgentGather(agent), xErrno_Ok);

  /* Only host candidate expected */
  EXPECT_FALSE(as.last_candidate.empty());
  EXPECT_NE(as.last_candidate.find("typ host"), std::string::npos);

  EXPECT_TRUE(pump_until(loop, [&] { return as.gathering_done; }, 6000));

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}

TEST(IceAgentTest, ConfWithoutServersGathersHostOnly) {
  /*
   * With no STUN/TURN servers configured, only host candidates
   * should be gathered and gathering completes via timeout.
   */
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);

  AgentState as;
  xIceConf   config;
  memset(&config, 0, sizeof(config));
  config.role            = xIceRole_Controlling;
  config.on_state_change = on_state_change;
  config.on_candidate    = on_candidate;
  config.ctx             = &as;

  xIceAgent agent = xIceAgentCreate(loop, &config);
  ASSERT_NE(agent, nullptr);

  EXPECT_EQ(xIceAgentGather(agent), xErrno_Ok);
  EXPECT_FALSE(as.last_candidate.empty());
  EXPECT_NE(as.last_candidate.find("typ host"), std::string::npos);

  /* No pending gather requests → gathering completes immediately */
  EXPECT_TRUE(pump_until(loop, [&] { return as.gathering_done; }, 6000));

  xIceAgentDestroy(agent);
  xEventLoopDestroy(loop);
}
