/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * peer_connection.c - WebRTC PeerConnection orchestration
 *
 * Owns and drives the protocol stack:
 *   xIceAgent → xDtlsTransport → xSctpTransport → xDataChannelMgr
 */

#include "peer_connection.h"
#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ───────────────────── Internal Structure ───────────────────── */

XDEF_STRUCT(xPeerConnection_) {
  xPeerConnectionConf conf;
  xEventLoop          loop;
  xPeerConnectionState state;

  /* Protocol stack (owned) */
  xIceAgent       ice;
  xDtlsTransport  dtls;
  xSctpTransport  sctp;
  xDataChannelMgr dc_mgr;

  /* Negotiation state */
  bool     is_offerer;       /**< true if we created the offer.        */
  bool     gathering_started;
  bool     remote_set;

  /* Remote DTLS parameters (parsed from remote SDP) */
  char     remote_fingerprint[XSDP_MAX_FINGERPRINT_LEN];
  int      remote_setup;     /**< xIceSdpSetup value from remote SDP.  */
  uint16_t remote_sctp_port;

  /* Local DTLS role (derived from negotiation) */
  xDtlsRole dtls_role;
  uint16_t  local_sctp_port;

  /* Local fingerprint cached from certificate generation.
   * Stored as "sha-256 XX:XX:..." so it can be reused when the real
   * DTLS transport is created after ICE connects. */
  char local_fingerprint_str[XSDP_MAX_FINGERPRINT_LEN];

  /* Pending DataChannels (created before SCTP is ready) */
  xDataChannelConf pending_channels[XDC_MAX_CHANNELS];
  int              pending_channel_count;
};

/* ───────────────────── Helpers ───────────────────── */

static void set_state(xPeerConnection_ *pc, xPeerConnectionState new_state) {
  if (pc->state == new_state) return;
  pc->state = new_state;
  if (pc->conf.on_state_change) {
    pc->conf.on_state_change((xPeerConnection)pc, new_state, pc->conf.ctx);
  }
}

/* ───────────────────── Forward Declarations ───────────────────── */

/* DTLS ↔ ICE glue */
static void   pc_ice_dtls_input(const uint8_t *data, size_t len,
                                const struct sockaddr *from, void *arg);
static xErrno pc_dtls_send(const uint8_t *data, size_t len, void *arg);

/* DTLS callbacks */
static void pc_dtls_state_changed(xDtlsTransport transport, xDtlsState state,
                                  void *arg);
static void pc_dtls_data_received(xDtlsTransport transport,
                                  const uint8_t *data, size_t len, void *arg);

/* SCTP callbacks */
static void pc_sctp_state_changed(xSctpTransport transport, bool connected,
                                  void *arg);
static void pc_sctp_data_received(xSctpTransport transport, uint16_t stream_id,
                                  uint32_t ppid, const uint8_t *data,
                                  size_t len, void *arg);
static void pc_sctp_stream_closed(xSctpTransport transport,
                                  uint16_t stream_id, void *arg);

/* DataChannel callback */
static void pc_on_remote_datachannel(xDataChannelMgr mgr,
                                     xDataChannel channel, void *arg);

/* ───────────────────── ICE Callbacks ───────────────────── */

static void on_ice_state_change(xIceAgent agent, xIceState state, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)agent;

  switch (state) {
  case xIceState_Checking:
    set_state(pc, xPeerConnectionState_Connecting);
    break;

  case xIceState_Connected:
  case xIceState_Completed:
    /* ICE connected — create (or reuse) DTLS transport and start handshake.
     *
     * The DTLS transport is intentionally NOT created during createOffer
     * because the final role (Active vs Passive) is only known after
     * setRemoteDescription.  We cache the local fingerprint string and
     * create the real transport here with the correct role. */
    if (!pc->dtls) {
      uint8_t remote_fp[XDTLS_FINGERPRINT_SIZE];
      bool    verify_fp = false;
      memset(remote_fp, 0, sizeof(remote_fp));
      if (pc->remote_fingerprint[0] != '\0') {
        const char *fp_str = pc->remote_fingerprint;
        if (strncmp(fp_str, "sha-256 ", 8) == 0) fp_str += 8;
        if (xDtlsFingerprintFromStr(fp_str, remote_fp) == xErrno_Ok) {
          verify_fp = true;
        }
      }

      xDtlsRole role = pc->dtls_role;
      if (role == xDtlsRole_Actpass) role = xDtlsRole_Passive;

      fprintf(stderr, "[dtls] creating transport role=%s\n",
              role == xDtlsRole_Active ? "active" : "passive");

      xDtlsTransportConf dtls_conf;
      memset(&dtls_conf, 0, sizeof(dtls_conf));
      dtls_conf.loop               = pc->loop;
      dtls_conf.role               = role;
      dtls_conf.verify_fingerprint = verify_fp;
      memcpy(dtls_conf.remote_fingerprint, remote_fp, XDTLS_FINGERPRINT_SIZE);
      dtls_conf.send_fn         = pc_dtls_send;
      dtls_conf.send_arg        = pc;
      dtls_conf.on_state_change = pc_dtls_state_changed;
      dtls_conf.on_data         = pc_dtls_data_received;
      dtls_conf.ctx             = pc;

      pc->dtls = xDtlsTransportCreate(&dtls_conf);
      if (!pc->dtls) {
        set_state(pc, xPeerConnectionState_Failed);
        return;
      }
    }

    xIceAgentSetDtlsInputCallback(pc->ice, pc_ice_dtls_input, pc);
    if (xDtlsTransportGetState(pc->dtls) == xDtlsState_New) {
      xDtlsTransportStart(pc->dtls);
    }
    break;

  case xIceState_Failed:
    set_state(pc, xPeerConnectionState_Failed);
    break;

  case xIceState_Closed:
    set_state(pc, xPeerConnectionState_Closed);
    break;

  default:
    break;
  }
}

static void on_ice_candidate(xIceAgent agent, const char *candidate_sdp,
                             void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)agent;

  if (pc->conf.on_ice_candidate) {
    pc->conf.on_ice_candidate((xPeerConnection)pc, candidate_sdp, pc->conf.ctx);
  }
}

/* ───────────────────── DTLS ↔ ICE Glue ───────────────────── */

/**
 * @brief ICE demux feeds DTLS packets into the DTLS transport.
 */
static void pc_ice_dtls_input(const uint8_t *data, size_t len,
                              const struct sockaddr *from, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)from;
  if (pc->dtls) {
    xDtlsTransportFeedInput(pc->dtls, data, len);
  }
}

/**
 * @brief DTLS transport sends encrypted records through ICE.
 */
static xErrno pc_dtls_send(const uint8_t *data, size_t len, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  return xIceAgentSend(pc->ice, data, len);
}

/* ───────────────────── DTLS Callbacks ───────────────────── */

static void pc_dtls_state_changed(xDtlsTransport transport, xDtlsState state,
                                  void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)transport;

  if (state == xDtlsState_Connected) {
    /* DTLS connected — start SCTP */
    uint16_t local_port  = pc->local_sctp_port  ? pc->local_sctp_port  : XSCTP_DEFAULT_PORT;
    uint16_t remote_port = pc->remote_sctp_port ? pc->remote_sctp_port : XSCTP_DEFAULT_PORT;

    xSctpTransportConf sctp_conf;
    memset(&sctp_conf, 0, sizeof(sctp_conf));
    sctp_conf.loop        = pc->loop;
    sctp_conf.dtls        = pc->dtls;
    sctp_conf.is_client   = (pc->dtls_role == xDtlsRole_Active);
    sctp_conf.local_port  = local_port;
    sctp_conf.remote_port = remote_port;
    sctp_conf.on_state_change = pc_sctp_state_changed;
    sctp_conf.on_data         = pc_sctp_data_received;
    sctp_conf.on_stream_close = pc_sctp_stream_closed;
    sctp_conf.ctx             = pc;

    pc->sctp = xSctpTransportCreate(&sctp_conf);
    if (pc->sctp) {
      xSctpTransportStart(pc->sctp);
    } else {
      set_state(pc, xPeerConnectionState_Failed);
    }
  } else if (state == xDtlsState_Failed) {
    set_state(pc, xPeerConnectionState_Failed);
  }
}

static void pc_dtls_data_received(xDtlsTransport transport,
                                  const uint8_t *data, size_t len, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)transport;
  if (pc->sctp) {
    xSctpTransportFeedInput(pc->sctp, data, len);
  }
}

/* ───────────────────── SCTP Callbacks ───────────────────── */

static void pc_sctp_state_changed(xSctpTransport transport, bool connected,
                                  void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)transport;

  if (connected) {
    /* SCTP connected — create DataChannel manager */
    xDataChannelMgrConf dc_conf;
    memset(&dc_conf, 0, sizeof(dc_conf));
    dc_conf.sctp       = pc->sctp;
    dc_conf.on_remote_open = pc_on_remote_datachannel;
    dc_conf.on_open    = pc->conf.on_dc_open;
    dc_conf.on_message = pc->conf.on_dc_message;
    dc_conf.on_close   = pc->conf.on_dc_close;
    dc_conf.ctx        = pc->conf.ctx;

    pc->dc_mgr = xDataChannelMgrCreate(&dc_conf);

    /* Open any pending DataChannels */
    for (int i = 0; i < pc->pending_channel_count; i++) {
      xDataChannelCreate(pc->dc_mgr, &pc->pending_channels[i]);
    }
    pc->pending_channel_count = 0;

    set_state(pc, xPeerConnectionState_Connected);
  }
}

static void pc_sctp_data_received(xSctpTransport transport, uint16_t stream_id,
                                  uint32_t ppid, const uint8_t *data,
                                  size_t len, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)transport;
  if (pc->dc_mgr) {
    xDataChannelMgrOnData(pc->dc_mgr, stream_id, ppid, data, len);
  }
}

static void pc_sctp_stream_closed(xSctpTransport transport,
                                  uint16_t stream_id, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)transport;
  if (pc->dc_mgr) {
    xDataChannelMgrOnStreamClose(pc->dc_mgr, stream_id);
  }
}

/* ───────────────────── DataChannel Callback ───────────────────── */

static void pc_on_remote_datachannel(xDataChannelMgr mgr,
                                     xDataChannel channel, void *arg) {
  xPeerConnection_ *pc = (xPeerConnection_ *)arg;
  (void)mgr;
  if (pc->conf.on_datachannel) {
    pc->conf.on_datachannel((xPeerConnection)pc, channel, pc->conf.ctx);
  }
}

/* ───────────────────── Public API ───────────────────── */

xPeerConnection xPeerConnectionCreate(xEventLoop                 loop,
                                      const xPeerConnectionConf *conf) {
  if (!loop || !conf) return NULL;

  xPeerConnection_ *pc = (xPeerConnection_ *)calloc(1, sizeof(xPeerConnection_));
  if (!pc) return NULL;

  pc->conf  = *conf;
  pc->loop  = loop;
  pc->state = xPeerConnectionState_New;
  pc->local_sctp_port = conf->sctp_port ? conf->sctp_port : XSCTP_DEFAULT_PORT;

  /* Create ICE agent — pure ICE, no DTLS pollution */
  xIceConf ice_conf;
  memset(&ice_conf, 0, sizeof(ice_conf));
  ice_conf.role            = xIceRole_Controlling; /* Will be set by offer/answer */
  ice_conf.enable_ipv6     = conf->enable_ipv6;
  ice_conf.stun_server     = conf->stun_server;
  ice_conf.turn_server     = conf->turn_server;
  ice_conf.turn_username   = conf->turn_username;
  ice_conf.turn_password   = conf->turn_password;
  ice_conf.on_state_change = on_ice_state_change;
  ice_conf.on_candidate    = on_ice_candidate;
  ice_conf.on_data         = NULL; /* Raw data goes through DTLS, not directly */
  ice_conf.ctx             = pc;

  pc->ice = xIceAgentCreate(loop, &ice_conf);
  if (!pc->ice) {
    free(pc);
    return NULL;
  }

  return (xPeerConnection)pc;
}

void xPeerConnectionDestroy(xPeerConnection handle) {
  if (!handle) return;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;

  /* Tear down in reverse order */
  if (pc->dc_mgr) {
    xDataChannelMgrDestroy(pc->dc_mgr);
    pc->dc_mgr = NULL;
  }
  if (pc->sctp) {
    xSctpTransportDestroy(pc->sctp);
    pc->sctp = NULL;
  }
  if (pc->dtls) {
    xDtlsTransportDestroy(pc->dtls);
    pc->dtls = NULL;
  }
  if (pc->ice) {
    xIceAgentDestroy(pc->ice);
    pc->ice = NULL;
  }

  set_state(pc, xPeerConnectionState_Closed);
  free(pc);
}

char *xPeerConnectionCreateOffer(xPeerConnection handle) {
  if (!handle) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;

  pc->is_offerer = true;
  pc->dtls_role  = xDtlsRole_Actpass; /* Offerer defaults to actpass */

  /* Start gathering if not already started */
  if (!pc->gathering_started) {
    xIceAgentGather(pc->ice);
    pc->gathering_started = true;
  }

  /* Generate a certificate just to obtain the local fingerprint.
   * We do NOT keep this transport — the real one will be created in
   * on_ice_state_change once we know the final DTLS role from the answer. */
  if (pc->local_fingerprint_str[0] == '\0') {
    xDtlsTransportConf dtls_conf;
    memset(&dtls_conf, 0, sizeof(dtls_conf));
    dtls_conf.loop     = pc->loop;
    dtls_conf.role     = xDtlsRole_Passive; /* role doesn't matter for cert */
    dtls_conf.send_fn  = pc_dtls_send;
    dtls_conf.send_arg = pc;

    xDtlsTransport tmp = xDtlsTransportCreate(&dtls_conf);
    if (!tmp) return NULL;

    char fp_str[XDTLS_FINGERPRINT_STR_SIZE];
    xErrno fp_err = xDtlsTransportGetFingerprintStr(tmp, fp_str);
    xDtlsTransportDestroy(tmp);
    if (fp_err != xErrno_Ok) return NULL;

    snprintf(pc->local_fingerprint_str, sizeof(pc->local_fingerprint_str),
             "sha-256 %s", fp_str);
  }

  const char *fp_full = pc->local_fingerprint_str;

  /* Get ICE credentials and gathered candidates */
  const char *ufrag = xIceAgentGetUfrag(pc->ice);
  const char *pwd   = xIceAgentGetPwd(pc->ice);

  int cand_count = 0;
  const xIceCandidate *candidates =
    xIceAgentGetLocalCandidates(pc->ice, &cand_count);

  char *sdp = (char *)malloc(XSDP_MAX_SIZE);
  if (!sdp) return NULL;

  int len = xIceSdpEncodeWebRTC(
    ufrag, pwd, candidates, cand_count, true,
    fp_full, xIceSdpSetup_Actpass, "0", pc->local_sctp_port,
    sdp, XSDP_MAX_SIZE);
  if (len < 0) {
    free(sdp);
    return NULL;
  }
  sdp[len] = '\0';
  return sdp;
}

char *xPeerConnectionCreateAnswer(xPeerConnection handle) {
  if (!handle) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;

  pc->is_offerer = false;

  /* Answerer determines role from remote setup */
  if (pc->remote_setup == (int)xIceSdpSetup_Actpass ||
      pc->remote_setup == (int)xIceSdpSetup_Active) {
    pc->dtls_role = xDtlsRole_Passive;
  } else {
    pc->dtls_role = xDtlsRole_Active;
  }

  /* Start gathering if not already started */
  if (!pc->gathering_started) {
    xIceAgentGather(pc->ice);
    pc->gathering_started = true;
  }

  /* Generate a certificate just to obtain the local fingerprint.
   * The real DTLS transport (with correct role + remote fingerprint) will be
   * created in on_ice_state_change after ICE connects. */
  if (pc->local_fingerprint_str[0] == '\0') {
    xDtlsTransportConf dtls_conf;
    memset(&dtls_conf, 0, sizeof(dtls_conf));
    dtls_conf.loop     = pc->loop;
    dtls_conf.role     = xDtlsRole_Passive; /* role doesn't matter for cert */
    dtls_conf.send_fn  = pc_dtls_send;
    dtls_conf.send_arg = pc;

    xDtlsTransport tmp = xDtlsTransportCreate(&dtls_conf);
    if (!tmp) return NULL;

    char fp_str[XDTLS_FINGERPRINT_STR_SIZE];
    xErrno fp_err = xDtlsTransportGetFingerprintStr(tmp, fp_str);
    xDtlsTransportDestroy(tmp);
    if (fp_err != xErrno_Ok) return NULL;

    snprintf(pc->local_fingerprint_str, sizeof(pc->local_fingerprint_str),
             "sha-256 %s", fp_str);
  }

  const char *fp_full = pc->local_fingerprint_str;

  const char *ufrag = xIceAgentGetUfrag(pc->ice);
  const char *pwd   = xIceAgentGetPwd(pc->ice);

  int cand_count = 0;
  const xIceCandidate *candidates =
    xIceAgentGetLocalCandidates(pc->ice, &cand_count);

  /* Answer setup role */
  xIceSdpSetup setup;
  if (pc->dtls_role == xDtlsRole_Active) {
    setup = xIceSdpSetup_Active;
  } else {
    setup = xIceSdpSetup_Passive;
  }

  char *sdp = (char *)malloc(XSDP_MAX_SIZE);
  if (!sdp) return NULL;

  int len = xIceSdpEncodeWebRTC(
    ufrag, pwd, candidates, cand_count, true,
    fp_full, setup, "0", pc->local_sctp_port,
    sdp, XSDP_MAX_SIZE);
  if (len < 0) {
    free(sdp);
    return NULL;
  }
  sdp[len] = '\0';
  return sdp;
}

xErrno xPeerConnectionSetLocalDescription(xPeerConnection handle,
                                          const char     *sdp) {
  if (!handle || !sdp) return xErrno_InvalidArg;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;

  /* Start gathering if not already started */
  if (!pc->gathering_started) {
    xIceAgentGather(pc->ice);
    pc->gathering_started = true;
  }

  return xErrno_Ok;
}

xErrno xPeerConnectionSetRemoteDescription(xPeerConnection handle,
                                           const char     *sdp) {
  if (!handle || !sdp) return xErrno_InvalidArg;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;

  /* Parse the SDP to extract WebRTC parameters */
  xIceSdp parsed;
  xErrno  err = xIceSdpDecode(sdp, strlen(sdp), &parsed);
  if (err != xErrno_Ok) return err;

  /* Store remote DTLS parameters */
  if (parsed.is_webrtc) {
    if (parsed.fingerprint[0] != '\0') {
      strncpy(pc->remote_fingerprint, parsed.fingerprint,
              XSDP_MAX_FINGERPRINT_LEN - 1);
    }
    pc->remote_setup     = (int)parsed.setup;
    pc->remote_sctp_port = parsed.sctp_port;

    /* Determine DTLS role from remote setup */
    if (pc->is_offerer) {
      /* We are offerer (actpass), remote is answerer */
      if (parsed.setup == xIceSdpSetup_Active) {
        pc->dtls_role = xDtlsRole_Passive;
      } else {
        pc->dtls_role = xDtlsRole_Active;
      }
    } else {
      /* We are answerer */
      if (parsed.setup == xIceSdpSetup_Actpass ||
          parsed.setup == xIceSdpSetup_Active) {
        pc->dtls_role = xDtlsRole_Passive;
      } else {
        pc->dtls_role = xDtlsRole_Active;
      }
    }
  }

  pc->remote_set = true;

  /* Forward ICE-level SDP to the ICE agent */
  return xIceAgentSetRemoteDescription(pc->ice, sdp);
}

xErrno xPeerConnectionAddIceCandidate(xPeerConnection handle,
                                      const char     *candidate_sdp) {
  if (!handle || !candidate_sdp) return xErrno_InvalidArg;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;
  return xIceAgentAddRemoteCandidate(pc->ice, candidate_sdp);
}

xDataChannel xPeerConnectionCreateDataChannel(xPeerConnection         handle,
                                              const xDataChannelConf *conf) {
  if (!handle || !conf) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;

  /* If SCTP is ready, create immediately */
  if (pc->dc_mgr) {
    return xDataChannelCreate(pc->dc_mgr, conf);
  }

  /* Otherwise queue for later */
  if (pc->pending_channel_count >= XDC_MAX_CHANNELS) return NULL;
  pc->pending_channels[pc->pending_channel_count++] = *conf;
  return NULL; /* Will be created when SCTP connects */
}

/* ───────────────────── Accessors ───────────────────── */

xPeerConnectionState xPeerConnectionGetState(xPeerConnection handle) {
  if (!handle) return xPeerConnectionState_Closed;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;
  return pc->state;
}

xIceAgent xPeerConnectionGetIceAgent(xPeerConnection handle) {
  if (!handle) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;
  return pc->ice;
}

xDtlsTransport xPeerConnectionGetDtlsTransport(xPeerConnection handle) {
  if (!handle) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;
  return pc->dtls;
}

xSctpTransport xPeerConnectionGetSctpTransport(xPeerConnection handle) {
  if (!handle) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;
  return pc->sctp;
}

xDataChannelMgr xPeerConnectionGetDataChannelMgr(xPeerConnection handle) {
  if (!handle) return NULL;
  xPeerConnection_ *pc = (xPeerConnection_ *)handle;
  return pc->dc_mgr;
}
