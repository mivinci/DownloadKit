/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * xfer_signal_server.c - Signaling server for P2P file transfer
 *
 * A lightweight WebSocket-based signaling server that pairs sender
 * and receiver peers and relays SDP/ICE messages between them.
 *
 * Protocol (JSON over WebSocket text frames):
 *
 *   Sender  → Server:  {"type":"create"}
 *   Server  → Sender:  {"type":"code","code":"abc123"}
 *   Receiver→ Server:  {"type":"join","code":"abc123"}
 *   Server  → Receiver:{"type":"joined"}
 *   Server  → Sender:  {"type":"peer_joined"}
 *   Sender  → Server:  {"type":"offer","sdp":"..."}
 *   Server  → Receiver:{"type":"offer","sdp":"..."}
 *   Receiver→ Server:  {"type":"answer","sdp":"..."}
 *   Server  → Sender:  {"type":"answer","sdp":"..."}
 *   Either  → Server:  {"type":"candidate","candidate":"..."}
 *   Server  → Peer:    {"type":"candidate","candidate":"..."}
 *   Either  → Server:  {"type":"heartbeat"}
 *   Server  → Either:  {"type":"heartbeat"}
 */

#include "xfer_signal.h"

#include <x/base/log.h>
#include <x/http/server.h>
#include <x/http/ws.h>

#include <cJSON.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Constants ─────────────────────────────────────────── */

#define SIGNAL_MAX_SESSIONS 256
#define SIGNAL_CODE_LEN     6

/* ── Session ───────────────────────────────────────────── */

XDEF_STRUCT(xSignalSession) {
  char    code[SIGNAL_CODE_LEN + 1];
  xWsConn sender;
  xWsConn receiver;
  bool    active;
};

/* ── Server internal state ─────────────────────────────── */

XDEF_STRUCT(xSignalServer_) {
  xEventLoop     loop;
  xHttpServer    http;
  xSignalSession sessions[SIGNAL_MAX_SESSIONS];
  int            session_count;
};

/* ── Helpers ───────────────────────────────────────────── */

static void generate_code(char *buf, int len) {
  static const char charset[] =
    "abcdefghijklmnopqrstuvwxyz0123456789";
  static bool seeded = false;
  if (!seeded) {
    srand((unsigned)time(NULL));
    seeded = true;
  }
  for (int i = 0; i < len; i++) {
    buf[i] = charset[rand() % (sizeof(charset) - 1)];
  }
  buf[len] = '\0';
}

static xSignalSession *find_session_by_code(xSignalServer_ *srv,
                                            const char *code) {
  for (int i = 0; i < srv->session_count; i++) {
    if (srv->sessions[i].active &&
        strcmp(srv->sessions[i].code, code) == 0) {
      return &srv->sessions[i];
    }
  }
  return NULL;
}

static xSignalSession *find_session_by_conn(xSignalServer_ *srv,
                                            xWsConn conn) {
  for (int i = 0; i < srv->session_count; i++) {
    if (srv->sessions[i].active &&
        (srv->sessions[i].sender == conn ||
         srv->sessions[i].receiver == conn)) {
      return &srv->sessions[i];
    }
  }
  return NULL;
}

static xWsConn get_peer(xSignalSession *session, xWsConn conn) {
  if (session->sender == conn) return session->receiver;
  if (session->receiver == conn) return session->sender;
  return NULL;
}

static void send_json(xWsConn conn, cJSON *json) {
  char *str = cJSON_PrintUnformatted(json);
  if (str) {
    xWsSend(conn, xWsOpcode_Text, str, strlen(str));
    free(str);
  }
}

/* ── WebSocket callbacks ───────────────────────────────── */

static void on_ws_open(xWsConn conn, void *arg) {
  (void)conn;
  (void)arg;
  XDEBUG("[signal-server] new WebSocket connection");
}

static void on_ws_message(xWsConn conn, xWsOpcode opcode,
                          const void *payload, size_t len, void *arg) {
  xSignalServer_ *srv = (xSignalServer_ *)arg;
  (void)opcode;

  /* Parse JSON message */
  cJSON *json = cJSON_ParseWithLength((const char *)payload, len);
  if (!json) {
    XDEBUG("[signal-server] failed to parse JSON message");
    return;
  }

  cJSON *type_item = cJSON_GetObjectItemCaseSensitive(json, "type");
  if (!cJSON_IsString(type_item)) {
    cJSON_Delete(json);
    return;
  }

  const char *type = type_item->valuestring;

  if (strcmp(type, "create") == 0) {
    /* Sender wants to create a new session */
    /* Reuse an inactive slot first, then append */
    xSignalSession *session = NULL;
    for (int i = 0; i < srv->session_count; i++) {
      if (!srv->sessions[i].active) {
        session = &srv->sessions[i];
        break;
      }
    }
    if (!session) {
      if (srv->session_count >= SIGNAL_MAX_SESSIONS) {
        XDEBUG("[signal-server] max sessions reached");
        cJSON_Delete(json);
        return;
      }
      session = &srv->sessions[srv->session_count++];
    }
    memset(session, 0, sizeof(*session));
    session->sender = conn;

    /* Generate unique code (session is still inactive during check) */
    int attempts = 0;
    do {
      generate_code(session->code, SIGNAL_CODE_LEN);
      attempts++;
    } while (find_session_by_code(srv, session->code) != NULL &&
             attempts < 100);

    session->active = true;

    XDEBUG("[signal-server] session created: code=%s", session->code);

    /* Send code back to sender */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "code");
    cJSON_AddStringToObject(resp, "code", session->code);
    send_json(conn, resp);
    cJSON_Delete(resp);

  } else if (strcmp(type, "join") == 0) {
    /* Receiver wants to join a session */
    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(json, "code");
    if (!cJSON_IsString(code_item)) {
      cJSON_Delete(json);
      return;
    }

    xSignalSession *session = find_session_by_code(srv, code_item->valuestring);
    if (!session) {
      XDEBUG("[signal-server] join: session not found for code=%s",
             code_item->valuestring);
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "type", "error");
      cJSON_AddStringToObject(resp, "message", "session not found");
      send_json(conn, resp);
      cJSON_Delete(resp);
      cJSON_Delete(json);
      return;
    }

    if (session->receiver) {
      XDEBUG("[signal-server] join: session already has a receiver");
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "type", "error");
      cJSON_AddStringToObject(resp, "message", "session already has a receiver");
      send_json(conn, resp);
      cJSON_Delete(resp);
      cJSON_Delete(json);
      return;
    }

    session->receiver = conn;
    XDEBUG("[signal-server] receiver joined session: code=%s", session->code);

    /* Notify receiver */
    cJSON *joined = cJSON_CreateObject();
    cJSON_AddStringToObject(joined, "type", "joined");
    send_json(conn, joined);
    cJSON_Delete(joined);

    /* Notify sender */
    if (session->sender) {
      cJSON *peer_joined = cJSON_CreateObject();
      cJSON_AddStringToObject(peer_joined, "type", "peer_joined");
      send_json(session->sender, peer_joined);
      cJSON_Delete(peer_joined);
    }

  } else if (strcmp(type, "heartbeat") == 0) {
    /* Reply with a heartbeat to keep the connection alive */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "heartbeat");
    send_json(conn, resp);
    cJSON_Delete(resp);

  } else if (strcmp(type, "offer") == 0 ||
             strcmp(type, "answer") == 0 ||
             strcmp(type, "candidate") == 0) {
    /* Relay message to the peer */
    xSignalSession *session = find_session_by_conn(srv, conn);
    if (!session) {
      XDEBUG("[signal-server] relay: no session for this connection");
      cJSON_Delete(json);
      return;
    }

    xWsConn peer = get_peer(session, conn);
    if (!peer) {
      XDEBUG("[signal-server] relay: peer not connected yet");
      cJSON_Delete(json);
      return;
    }

    /* Forward the entire JSON message as-is */
    xWsSend(peer, xWsOpcode_Text, payload, len);

  } else {
    XDEBUG("[signal-server] unknown message type: %s", type);
  }

  cJSON_Delete(json);
}

static void on_ws_close(xWsConn conn, uint16_t code, const char *reason,
                        size_t len, void *arg) {
  xSignalServer_ *srv = (xSignalServer_ *)arg;
  (void)code;
  (void)reason;
  (void)len;

  /* Clean up session if this connection was part of one */
  xSignalSession *session = find_session_by_conn(srv, conn);
  if (session) {
    XDEBUG("[signal-server] connection closed for session: code=%s",
           session->code);
    session->active = false;
    session->sender = NULL;
    session->receiver = NULL;
  }
}

/* ── HTTP handler for WebSocket upgrade ────────────────── */

static void ws_handler(xHttpResponseWriter writer, const xHttpRequest *req,
                       void *arg) {
  xSignalServer_ *srv = (xSignalServer_ *)arg;

  xWsCallbacks cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_open    = on_ws_open;
  cbs.on_message = on_ws_message;
  cbs.on_close   = on_ws_close;

  xErrno err = xWsUpgrade(writer, req, &cbs, srv);
  if (err != xErrno_Ok) {
    XDEBUG("[signal-server] WebSocket upgrade failed: %d", err);
  }
}

/* ── Public API ────────────────────────────────────────── */

xSignalServer xSignalServerCreate(xEventLoop loop,
                                  const xSignalServerConf *conf) {
  if (!loop || !conf) return NULL;

  xSignalServer_ *srv = (xSignalServer_ *)calloc(1, sizeof(xSignalServer_));
  if (!srv) return NULL;

  srv->loop = loop;

  /* Create HTTP server */
  srv->http = xHttpServerCreate(loop);
  if (!srv->http) {
    free(srv);
    return NULL;
  }

  /* Register WebSocket route */
  xHttpServerRoute(srv->http, "/ws", ws_handler, srv);

  /* Start listening */
  xErrno err = xHttpServerListen(srv->http, conf->host, conf->port);
  if (err != xErrno_Ok) {
    xHttpServerDestroy(srv->http);
    free(srv);
    return NULL;
  }

  xLog(false, "[signal-server] listening on %s:%u",
       conf->host ? conf->host : "0.0.0.0", conf->port);

  return (xSignalServer)srv;
}

void xSignalServerDestroy(xSignalServer server) {
  if (!server) return;
  xSignalServer_ *srv = (xSignalServer_ *)server;

  if (srv->http) {
    xHttpServerDestroy(srv->http);
  }

  free(srv);
}
