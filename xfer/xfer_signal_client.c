/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_signal_client.c - Signaling client for P2P file transfer
 *
 * Connects to a signaling server via WebSocket and handles the
 * session creation/joining and SDP/ICE message exchange.
 */

#include "xfer_signal.h"

#include <xbase/log.h>
#include <xhttp/ws.h>

#include <cJSON.h>

#include <stdlib.h>
#include <string.h>

/* ── Internal state ────────────────────────────────────── */

XDEF_STRUCT(xSignalClient_) {
  xEventLoop        loop;
  xSignalClientConf conf;
  xWsConn           ws;
  bool              connected;
};

/* ── Helpers ───────────────────────────────────────────── */

static void send_json(xWsConn conn, cJSON *json) {
  char *str = cJSON_PrintUnformatted(json);
  if (str) {
    xWsSend(conn, xWsOpcode_Text, str, strlen(str));
    free(str);
  }
}

static void report_error(xSignalClient_ *impl, xErrno err, const char *msg) {
  if (impl->conf.on_error) {
    impl->conf.on_error((xSignalClient)impl, err, msg, impl->conf.ctx);
  }
}

/* ── WebSocket callbacks ───────────────────────────────── */

static void on_ws_open(xWsConn conn, void *arg) {
  xSignalClient_ *impl = (xSignalClient_ *)arg;
  impl->ws = conn;
  impl->connected = true;

  XDEBUG("[signal-client] connected to signaling server");

  if (impl->conf.role == xSignalClientRole_Sender) {
    /* Send "create" message */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "create");
    send_json(conn, msg);
    cJSON_Delete(msg);
  } else {
    /* Send "join" message with code */
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "join");
    cJSON_AddStringToObject(msg, "code",
                            impl->conf.code ? impl->conf.code : "");
    send_json(conn, msg);
    cJSON_Delete(msg);
  }
}

static void on_ws_message(xWsConn conn, xWsOpcode opcode,
                          const void *payload, size_t len, void *arg) {
  xSignalClient_ *impl = (xSignalClient_ *)arg;
  (void)conn;
  (void)opcode;

  cJSON *json = cJSON_ParseWithLength((const char *)payload, len);
  if (!json) {
    XDEBUG("[signal-client] failed to parse JSON message");
    return;
  }

  cJSON *type_item = cJSON_GetObjectItemCaseSensitive(json, "type");
  if (!cJSON_IsString(type_item)) {
    cJSON_Delete(json);
    return;
  }

  const char *type = type_item->valuestring;

  if (strcmp(type, "code") == 0) {
    /* Sender received a code */
    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(json, "code");
    if (cJSON_IsString(code_item) && impl->conf.on_code) {
      impl->conf.on_code((xSignalClient)impl, code_item->valuestring,
                         impl->conf.ctx);
    }
    if (impl->conf.on_connected) {
      impl->conf.on_connected((xSignalClient)impl, impl->conf.ctx);
    }

  } else if (strcmp(type, "joined") == 0) {
    /* Receiver successfully joined */
    if (impl->conf.on_connected) {
      impl->conf.on_connected((xSignalClient)impl, impl->conf.ctx);
    }

  } else if (strcmp(type, "peer_joined") == 0) {
    /* Sender: peer has joined */
    if (impl->conf.on_peer_joined) {
      impl->conf.on_peer_joined((xSignalClient)impl, impl->conf.ctx);
    }

  } else if (strcmp(type, "offer") == 0) {
    cJSON *sdp_item = cJSON_GetObjectItemCaseSensitive(json, "sdp");
    if (cJSON_IsString(sdp_item) && impl->conf.on_offer) {
      impl->conf.on_offer((xSignalClient)impl, sdp_item->valuestring,
                          impl->conf.ctx);
    }

  } else if (strcmp(type, "answer") == 0) {
    cJSON *sdp_item = cJSON_GetObjectItemCaseSensitive(json, "sdp");
    if (cJSON_IsString(sdp_item) && impl->conf.on_answer) {
      impl->conf.on_answer((xSignalClient)impl, sdp_item->valuestring,
                           impl->conf.ctx);
    }

  } else if (strcmp(type, "candidate") == 0) {
    cJSON *cand_item = cJSON_GetObjectItemCaseSensitive(json, "candidate");
    if (cJSON_IsString(cand_item) && impl->conf.on_candidate) {
      impl->conf.on_candidate((xSignalClient)impl, cand_item->valuestring,
                              impl->conf.ctx);
    }

  } else if (strcmp(type, "error") == 0) {
    cJSON *msg_item = cJSON_GetObjectItemCaseSensitive(json, "message");
    const char *msg = cJSON_IsString(msg_item) ? msg_item->valuestring
                                               : "unknown error";
    report_error(impl, xErrno_Unknown, msg);

  } else {
    XDEBUG("[signal-client] unknown message type: %s", type);
  }

  cJSON_Delete(json);
}

static void on_ws_close(xWsConn conn, uint16_t code, const char *reason,
                        size_t len, void *arg) {
  xSignalClient_ *impl = (xSignalClient_ *)arg;
  (void)conn;
  (void)code;
  (void)reason;
  (void)len;

  XDEBUG("[signal-client] WebSocket closed");
  impl->ws = NULL;
  impl->connected = false;
}

/* ── Public API ────────────────────────────────────────── */

xSignalClient xSignalClientCreate(xEventLoop loop,
                                  const xSignalClientConf *conf) {
  if (!loop || !conf || !conf->url) return NULL;

  xSignalClient_ *impl = (xSignalClient_ *)calloc(1, sizeof(xSignalClient_));
  if (!impl) return NULL;

  impl->loop = loop;
  impl->conf = *conf;

  /* Initiate WebSocket connection */
  xWsConnectConf ws_conf;
  memset(&ws_conf, 0, sizeof(ws_conf));
  ws_conf.url = conf->url;

  xWsCallbacks cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_open    = on_ws_open;
  cbs.on_message = on_ws_message;
  cbs.on_close   = on_ws_close;

  xErrno err = xWsConnect(loop, &ws_conf, &cbs, impl);
  if (err != xErrno_Ok) {
    free(impl);
    return NULL;
  }

  return (xSignalClient)impl;
}

void xSignalClientDestroy(xSignalClient client) {
  if (!client) return;
  xSignalClient_ *impl = (xSignalClient_ *)client;

  if (impl->ws) {
    xWsClose(impl->ws, 1000);
    impl->ws = NULL;
  }

  free(impl);
}

xErrno xSignalClientSendOffer(xSignalClient client, const char *sdp) {
  if (!client || !sdp) return xErrno_InvalidArg;
  xSignalClient_ *impl = (xSignalClient_ *)client;
  if (!impl->ws) return xErrno_InvalidState;

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "offer");
  cJSON_AddStringToObject(msg, "sdp", sdp);
  send_json(impl->ws, msg);
  cJSON_Delete(msg);
  return xErrno_Ok;
}

xErrno xSignalClientSendAnswer(xSignalClient client, const char *sdp) {
  if (!client || !sdp) return xErrno_InvalidArg;
  xSignalClient_ *impl = (xSignalClient_ *)client;
  if (!impl->ws) return xErrno_InvalidState;

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "answer");
  cJSON_AddStringToObject(msg, "sdp", sdp);
  send_json(impl->ws, msg);
  cJSON_Delete(msg);
  return xErrno_Ok;
}

xErrno xSignalClientSendCandidate(xSignalClient client,
                                  const char *candidate) {
  if (!client || !candidate) return xErrno_InvalidArg;
  xSignalClient_ *impl = (xSignalClient_ *)client;
  if (!impl->ws) return xErrno_InvalidState;

  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type", "candidate");
  cJSON_AddStringToObject(msg, "candidate", candidate);
  send_json(impl->ws, msg);
  cJSON_Delete(msg);
  return xErrno_Ok;
}
