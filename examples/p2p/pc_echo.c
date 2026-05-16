/*
 * pc_echo.c - WebRTC DataChannel loopback echo demo
 *
 * Creates two PeerConnections with DTLS + SCTP + DataChannel,
 * exchanges WebRTC SDP between them, and once the DataChannel is open
 * the offerer sends "Hello DataChannel!" which the answerer echoes back.
 *
 * This demonstrates the full WebRTC DataChannel stack via xPeerConnection:
 *   ICE → DTLS → SCTP → DataChannel (DCEP)
 *
 * Usage:
 *   ./pc_echo [-s stun_server:port] [-6]
 *
 * Example:
 *   ./pc_echo
 *   ./pc_echo -s stun.l.google.com:19302
 *   ./pc_echo -6
 */

#include <x/base/event.h>
#include <x/p2p/peer_connection.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Globals ───────────────────────────────────────────── */

static xEventLoop      g_loop;
static xPeerConnection g_pc_a; /* Offerer  */
static xPeerConnection g_pc_b; /* Answerer */

static bool g_a_gathering_done = false;
static bool g_b_gathering_done = false;

static const char *g_stun_server = "stun.l.google.com:19302";
static bool        g_enable_ipv6 = false;

/* ── Forward declarations ──────────────────────────────── */

static void exchange_sdp(void);

/* ── PeerConnection Callbacks ──────────────────────────── */

static void on_state_change(xPeerConnection pc, xPeerConnectionState state,
                            void *ctx) {
  (void)pc;
  const char *name = (const char *)ctx;
  const char *state_str;
  switch (state) {
  case xPeerConnectionState_New:          state_str = "New";          break;
  case xPeerConnectionState_Connecting:   state_str = "Connecting";   break;
  case xPeerConnectionState_Connected:    state_str = "Connected";    break;
  case xPeerConnectionState_Disconnected: state_str = "Disconnected"; break;
  case xPeerConnectionState_Failed:       state_str = "Failed";       break;
  case xPeerConnectionState_Closed:       state_str = "Closed";       break;
  default:                                state_str = "Unknown";      break;
  }
  printf("[%s] State: %s\n", name, state_str);
}

static void on_ice_candidate(xPeerConnection pc, const char *candidate,
                             void *ctx) {
  const char *name = (const char *)ctx;
  if (candidate) {
    printf("[%s] ICE candidate: %s\n", name, candidate);
  } else {
    printf("[%s] ICE gathering done\n", name);
    if (pc == g_pc_a) g_a_gathering_done = true;
    if (pc == g_pc_b) g_b_gathering_done = true;

    if (g_a_gathering_done && g_b_gathering_done) {
      exchange_sdp();
    }
  }
}

/* ── DataChannel Callbacks ─────────────────────────────── */

static void on_datachannel(xPeerConnection pc, xDataChannel channel,
                           void *ctx) {
  (void)pc;
  const char *name = (const char *)ctx;
  printf("[%s] Remote DataChannel opened: label=\"%s\"\n", name,
         xDataChannelGetLabel(channel));
}

static void on_dc_open(xDataChannel channel, void *ctx) {
  const char *name = (const char *)ctx;
  printf("[%s] DataChannel open: label=\"%s\"\n", name,
         xDataChannelGetLabel(channel));

  /* Offerer sends a message when channel opens */
  if (strcmp(name, "PC-A") == 0) {
    const char *msg = "Hello DataChannel!";
    printf("[%s] Sending: %s\n", name, msg);
    xDataChannelSendString(channel, msg, strlen(msg));
  }
}

static void on_dc_message(xDataChannel channel, xDataChannelMsgType type,
                          const uint8_t *data, size_t len, void *ctx) {
  const char *name = (const char *)ctx;
  if (type == xDataChannelMsgType_String) {
    printf("[%s] Received string (%zu bytes): %.*s\n", name, len, (int)len,
           (const char *)data);

    /* Answerer echoes back */
    if (strcmp(name, "PC-B") == 0) {
      printf("[%s] Echoing back...\n", name);
      xDataChannelSendString(channel, (const char *)data, len);
    } else {
      /* Offerer received echo — success! */
      printf("\n✅ DataChannel echo successful!\n\n");
      xEventLoopStop(g_loop);
    }
  } else {
    printf("[%s] Received binary (%zu bytes)\n", name, len);
  }
}

static void on_dc_close(xDataChannel channel, void *ctx) {
  (void)channel;
  const char *name = (const char *)ctx;
  printf("[%s] DataChannel closed\n", name);
}

/* ── SDP Exchange ──────────────────────────────────────── */

static void exchange_sdp(void) {
  printf("\n── Exchanging SDP ──\n\n");

  char *offer = xPeerConnectionCreateOffer(g_pc_a);
  if (!offer) {
    fprintf(stderr, "Failed to create offer\n");
    return;
  }
  printf("Offer SDP:\n%s\n", offer);

  xPeerConnectionSetLocalDescription(g_pc_a, offer);
  xPeerConnectionSetRemoteDescription(g_pc_b, offer);

  char *answer = xPeerConnectionCreateAnswer(g_pc_b);
  if (!answer) {
    fprintf(stderr, "Failed to create answer\n");
    free(offer);
    return;
  }
  printf("Answer SDP:\n%s\n", answer);

  xPeerConnectionSetLocalDescription(g_pc_b, answer);
  xPeerConnectionSetRemoteDescription(g_pc_a, answer);

  free(offer);
  free(answer);
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  /* Parse arguments */
  int opt;
  while ((opt = getopt(argc, argv, "s:6")) != -1) {
    switch (opt) {
    case 's':
      g_stun_server = optarg;
      break;
    case '6':
      g_enable_ipv6 = true;
      break;
    default:
      fprintf(stderr, "Usage: %s [-s stun_server:port] [-6]\n", argv[0]);
      return 1;
    }
  }

  printf("DataChannel Echo Demo\n");
  printf("STUN server: %s\n", g_stun_server);
  printf("IPv6:        %s\n\n", g_enable_ipv6 ? "enabled" : "disabled");

  g_loop = xEventLoopCreate();

  /* ── Create PeerConnection A (offerer) ── */
  xPeerConnectionConf conf_a;
  memset(&conf_a, 0, sizeof(conf_a));
  conf_a.stun_server      = g_stun_server;
  conf_a.enable_ipv6      = g_enable_ipv6;
  conf_a.on_state_change  = on_state_change;
  conf_a.on_ice_candidate = on_ice_candidate;
  conf_a.on_datachannel   = on_datachannel;
  conf_a.on_dc_open       = on_dc_open;
  conf_a.on_dc_message    = on_dc_message;
  conf_a.on_dc_close      = on_dc_close;
  conf_a.ctx              = (void *)"PC-A";

  g_pc_a = xPeerConnectionCreate(g_loop, &conf_a);

  /* ── Create PeerConnection B (answerer) ── */
  xPeerConnectionConf conf_b;
  memset(&conf_b, 0, sizeof(conf_b));
  conf_b.stun_server      = g_stun_server;
  conf_b.enable_ipv6      = g_enable_ipv6;
  conf_b.on_state_change  = on_state_change;
  conf_b.on_ice_candidate = on_ice_candidate;
  conf_b.on_datachannel   = on_datachannel;
  conf_b.on_dc_open       = on_dc_open;
  conf_b.on_dc_message    = on_dc_message;
  conf_b.on_dc_close      = on_dc_close;
  conf_b.ctx              = (void *)"PC-B";

  g_pc_b = xPeerConnectionCreate(g_loop, &conf_b);

  /* ── Create a DataChannel on the offerer side ── */
  xDataChannelConf dc_conf;
  memset(&dc_conf, 0, sizeof(dc_conf));
  strncpy(dc_conf.label, "echo", XDC_MAX_LABEL_LEN - 1);
  dc_conf.ordered = true;

  xPeerConnectionCreateDataChannel(g_pc_a, &dc_conf);

  /* ── Start gathering (triggers SDP exchange when both are done) ── */
  xIceAgentGather(xPeerConnectionGetIceAgent(g_pc_a));
  xIceAgentGather(xPeerConnectionGetIceAgent(g_pc_b));

  /* ── Run event loop ── */
  xEventLoopRun(g_loop);

  /* ── Cleanup ── */
  xPeerConnectionDestroy(g_pc_a);
  xPeerConnectionDestroy(g_pc_b);
  xEventLoopDestroy(g_loop);

  return 0;
}
