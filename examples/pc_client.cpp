/*
 * pc_client.cpp - WebRTC PeerConnection client with WebSocket signaling
 *
 * Connects to a signaling server via WebSocket, creates a PeerConnection
 * with a DataChannel, and exchanges SDP/ICE with a remote peer (typically
 * a browser running pc_client.html).
 *
 * Signaling protocol (JSON over WebSocket):
 *   {"type":"offer","sdp":"..."}
 *   {"type":"answer","sdp":"..."}
 *   {"type":"candidate","candidate":"..."}
 *
 * Usage:
 *   ./pc_client [-u signal_url] [-s stun_server] [-6]
 *
 * Examples:
 *   ./pc_client
 *   ./pc_client -u ws://localhost:8080/signal
 *   ./pc_client -u ws://localhost:8080/signal -s stun.l.google.com:19302
 *   ./pc_client -6
 *
 * Test flow:
 *   1. Start pc_server: ./pc_server 8080
 *   2. Open http://localhost:8080/ in a browser
 *   3. Run: ./pc_client ws://localhost:8080/signal
 *   4. The client creates an offer, sends it via the signaling server,
 *      the browser creates an answer, and a DataChannel is established.
 *   5. Type messages in the terminal to send via DataChannel.
 */

#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xhttp/ws.h>
#include <xp2p/peer_connection.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

/* ── Global state ──────────────────────────────────────── */

static xEventLoop      g_loop = nullptr;
static xWsConn         g_ws   = nullptr;
static xPeerConnection g_pc   = nullptr;

static std::atomic<bool> g_ws_connected{false};
static std::atomic<bool> g_dc_open{false};
static std::atomic<bool> g_done{false};

static xDataChannel g_dc = nullptr;

static const char *g_stun_server  = "stun.l.google.com:19302";
static bool        g_enable_ipv6  = false;

/* ── Simple JSON helpers (no dependency) ───────────────── */

/*
 * Build a JSON message like: {"type":"offer","sdp":"..."}
 * Caller must free() the returned string.
 * Escapes newlines and quotes in the value.
 */
static char *json_msg(const char *type, const char *key, const char *value) {
  /* Calculate escaped length */
  size_t val_len     = strlen(value);
  size_t escaped_len = 0;
  for (size_t i = 0; i < val_len; i++) {
    if (value[i] == '"' || value[i] == '\\')
      escaped_len += 2;
    else if (value[i] == '\n')
      escaped_len += 2;
    else if (value[i] == '\r')
      escaped_len += 2;
    else if (value[i] == '\t')
      escaped_len += 2;
    else
      escaped_len += 1;
  }

  /* {"type":"<type>","<key>":"<escaped_value>"} */
  size_t buf_sz = 32 + strlen(type) + strlen(key) + escaped_len;
  char  *buf    = (char *)malloc(buf_sz);
  if (!buf) return nullptr;

  int off = snprintf(buf, buf_sz, "{\"type\":\"%s\",\"%s\":\"", type, key);

  for (size_t i = 0; i < val_len; i++) {
    switch (value[i]) {
    case '"':
      buf[off++] = '\\';
      buf[off++] = '"';
      break;
    case '\\':
      buf[off++] = '\\';
      buf[off++] = '\\';
      break;
    case '\n':
      buf[off++] = '\\';
      buf[off++] = 'n';
      break;
    case '\r':
      buf[off++] = '\\';
      buf[off++] = 'r';
      break;
    case '\t':
      buf[off++] = '\\';
      buf[off++] = 't';
      break;
    default:
      buf[off++] = value[i];
      break;
    }
  }

  buf[off++] = '"';
  buf[off++] = '}';
  buf[off]   = '\0';
  return buf;
}

/*
 * Extract a JSON string value by key from a simple JSON object.
 * Returns a malloc'd string, or NULL if not found.
 * Only handles flat objects with string values.
 */
static char *json_get(const char *json, const char *key) {
  /* Find "key":" */
  char pattern[256];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

  const char *start = strstr(json, pattern);
  if (!start) return nullptr;
  start += strlen(pattern);

  /* Find closing quote (handle escapes) */
  size_t cap    = 1024;
  char  *result = (char *)malloc(cap);
  if (!result) return nullptr;

  size_t out = 0;
  for (const char *p = start; *p && *p != '"'; p++) {
    if (*p == '\\' && *(p + 1)) {
      p++;
      switch (*p) {
      case 'n':
        result[out++] = '\n';
        break;
      case 'r':
        result[out++] = '\r';
        break;
      case 't':
        result[out++] = '\t';
        break;
      case '"':
        result[out++] = '"';
        break;
      case '\\':
        result[out++] = '\\';
        break;
      default:
        result[out++] = *p;
        break;
      }
    } else {
      result[out++] = *p;
    }
    if (out + 2 >= cap) {
      cap *= 2;
      result = (char *)realloc(result, cap);
      if (!result) return nullptr;
    }
  }
  result[out] = '\0';
  return result;
}

/* ── PeerConnection callbacks ──────────────────────────── */

static void on_pc_state_change(xPeerConnection pc, xPeerConnectionState state,
                               void *arg) {
  (void)pc;
  (void)arg;
  const char *s;
  switch (state) {
  case xPeerConnectionState_New:
    s = "New";
    break;
  case xPeerConnectionState_Connecting:
    s = "Connecting";
    break;
  case xPeerConnectionState_Connected:
    s = "Connected";
    break;
  case xPeerConnectionState_Disconnected:
    s = "Disconnected";
    break;
  case xPeerConnectionState_Failed:
    s = "Failed";
    break;
  case xPeerConnectionState_Closed:
    s = "Closed";
    break;
  default:
    s = "Unknown";
    break;
  }
  printf("[pc] state: %s\n", s);
}

static void on_ice_candidate(xPeerConnection pc, const char *candidate,
                             void *arg) {
  (void)pc;
  (void)arg;
  if (candidate) {
    printf("[pc] on ICE candidate: %s\n", candidate);
    /* Send candidate to remote peer via signaling.
     * xIceSdpEncodeCandidate produces "a=candidate:...\r\n".
     * Browsers expect the candidate string without the "a=" prefix
     * and without the trailing "\r\n", so strip them here. */
    const char *cand_str = candidate;
    if (strncmp(cand_str, "a=", 2) == 0) {
      cand_str += 2; /* skip "a=" */
    }
    /* Copy and strip trailing \r\n */
    char cand_buf[512];
    strncpy(cand_buf, cand_str, sizeof(cand_buf) - 1);
    cand_buf[sizeof(cand_buf) - 1] = '\0';
    size_t n                       = strlen(cand_buf);
    while (n > 0 && (cand_buf[n - 1] == '\r' || cand_buf[n - 1] == '\n')) {
      cand_buf[--n] = '\0';
    }

    if (g_ws && g_ws_connected) {
      char *msg = json_msg("candidate", "candidate", cand_buf);
      if (msg) {
        xWsSend(g_ws, xWsOpcode_Text, msg, strlen(msg));
        free(msg);
      }
    }
  } else {
    printf("[pc] ICE gathering complete\n");
  }
}

static void on_datachannel(xPeerConnection pc, xDataChannel channel,
                           void *arg) {
  (void)pc;
  (void)arg;
  printf("[pc] remote DataChannel: \"%s\"\n", xDataChannelGetLabel(channel));
}

/* ── DataChannel callbacks ─────────────────────────────── */

static void on_dc_open(xDataChannel channel, void *arg) {
  (void)arg;
  g_dc      = channel;
  g_dc_open = true;
  printf("[dc] DataChannel open: \"%s\"\n", xDataChannelGetLabel(channel));
  printf("[dc] type a message and press Enter "
         "(\"quit\" to close)\n");
}

static void on_dc_message(xDataChannel channel, xDataChannelMsgType type,
                          const uint8_t *data, size_t len, void *arg) {
  (void)channel;
  (void)arg;
  if (type == xDataChannelMsgType_String) {
    printf("[dc] received: %.*s\n", (int)len, (const char *)data);
  } else {
    printf("[dc] received binary (%zu bytes)\n", len);
  }
}

static void on_dc_close(xDataChannel channel, void *arg) {
  (void)channel;
  (void)arg;
  printf("[dc] DataChannel closed\n");
  g_dc_open = false;
  g_dc      = nullptr;
}

/* ── WebSocket callbacks (signaling) ───────────────────── */

static void ws_on_open(xWsConn conn, void *arg) {
  (void)arg;
  g_ws           = conn;
  g_ws_connected = true;
  printf("[ws] connected to signaling server\n");

  /* Create PeerConnection */
  xPeerConnectionConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.stun_server      = g_stun_server;
  conf.enable_ipv6      = g_enable_ipv6;
  conf.on_state_change  = on_pc_state_change;
  conf.on_ice_candidate = on_ice_candidate;
  conf.on_datachannel   = on_datachannel;
  conf.on_dc_open       = on_dc_open;
  conf.on_dc_message    = on_dc_message;
  conf.on_dc_close      = on_dc_close;
  conf.ctx              = nullptr;

  g_pc = xPeerConnectionCreate(g_loop, &conf);
  if (!g_pc) {
    fprintf(stderr, "[pc] failed to create PeerConnection\n");
    xWsClose(conn, 1011);
    return;
  }

  /* Create a DataChannel */
  xDataChannelConf dc_conf;
  memset(&dc_conf, 0, sizeof(dc_conf));
  strncpy(dc_conf.label, "echo", XDC_MAX_LABEL_LEN - 1);
  dc_conf.ordered = true;

  xPeerConnectionCreateDataChannel(g_pc, &dc_conf);

  /* Create and send offer first, so the remote peer can create
   * a PeerConnection before we start sending ICE candidates. */
  char *offer = xPeerConnectionCreateOffer(g_pc);
  if (!offer) {
    fprintf(stderr, "[pc] failed to create offer\n");
    return;
  }

  xPeerConnectionSetLocalDescription(g_pc, offer);

  printf("[ws] sending offer (%zu bytes)\n", strlen(offer));
  char *msg = json_msg("offer", "sdp", offer);
  if (msg) {
    xWsSend(conn, xWsOpcode_Text, msg, strlen(msg));
    free(msg);
  }
  free(offer);

  /* Gather ICE candidates after sending the offer, so the remote
   * peer already has a PeerConnection ready to accept them. */
  xIceAgentGather(xPeerConnectionGetIceAgent(g_pc));
}

static void ws_on_message(xWsConn conn, xWsOpcode opcode, const void *payload,
                          size_t len, void *arg) {
  (void)conn;
  (void)opcode;
  (void)arg;

  /* NUL-terminate for string operations */
  char *json = (char *)malloc(len + 1);
  if (!json) return;
  memcpy(json, payload, len);
  json[len] = '\0';

  char *type = json_get(json, "type");
  if (!type) {
    printf("[ws] received unknown message\n");
    free(json);
    return;
  }

  if (strcmp(type, "answer") == 0) {
    char *sdp = json_get(json, "sdp");
    if (sdp && g_pc) {
      printf("[ws] received answer (%zu bytes)\n", strlen(sdp));
      xPeerConnectionSetRemoteDescription(g_pc, sdp);
      free(sdp);
    }
  } else if (strcmp(type, "candidate") == 0) {
    char *candidate = json_get(json, "candidate");
    if (candidate && g_pc) {
      printf("[ws] received ICE candidate: %s\n", candidate);
      xPeerConnectionAddIceCandidate(g_pc, candidate);
      free(candidate);
    }
  } else if (strcmp(type, "offer") == 0) {
    /* Browser initiated — we create an answer */
    char *sdp = json_get(json, "sdp");
    if (sdp && g_pc) {
      printf("[ws] received offer, creating answer\n");
      xPeerConnectionSetRemoteDescription(g_pc, sdp);

      char *answer = xPeerConnectionCreateAnswer(g_pc);
      if (answer) {
        xPeerConnectionSetLocalDescription(g_pc, answer);
        char *msg = json_msg("answer", "sdp", answer);
        if (msg) {
          xWsSend(g_ws, xWsOpcode_Text, msg, strlen(msg));
          free(msg);
        }
        free(answer);
      }
      free(sdp);
    }
  } else {
    printf("[ws] unknown message type: %s\n", type);
  }

  free(type);
  free(json);
}

static void ws_on_close(xWsConn conn, uint16_t code, const char *reason,
                        size_t len, void *arg) {
  (void)conn;
  (void)arg;
  printf("[ws] disconnected (code=%u", code);
  if (reason && len > 0) {
    printf(", reason=%.*s", (int)len, reason);
  }
  printf(")\n");

  g_ws_connected = false;
  g_ws           = nullptr;
  g_done         = true;
  xEventLoopStop(g_loop);
}

static const xWsCallbacks ws_cbs = {
  .on_open    = ws_on_open,
  .on_message = ws_on_message,
  .on_close   = ws_on_close,
};

/* ── Stdin reader thread ───────────────────────────────── */

static void stdin_thread_func() {
  char line[4096];

  while (!g_done) {
    if (!fgets(line, sizeof(line), stdin)) {
      break;
    }

    /* Strip trailing newline */
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[--n] = '\0';
    }
    if (n == 0) continue;

    if (strcmp(line, "quit") == 0) {
      printf("[dc] closing...\n");
      g_done = true;
      xEventLoopStop(g_loop);
      break;
    }

    if (g_dc_open && g_dc) {
      xErrno err = xDataChannelSendString(g_dc, line, n);
      if (err != xErrno_Ok) {
        printf("[dc] send failed: %d\n", err);
      } else {
        printf("[dc] sent: %s\n", line);
      }
    } else {
      printf("[dc] DataChannel not open yet\n");
    }
  }
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  const char *signal_url = "ws://127.0.0.1:8080/signal";

  int opt;
  while ((opt = getopt(argc, argv, "u:s:6")) != -1) {
    switch (opt) {
    case 'u':
      signal_url = optarg;
      break;
    case 's':
      g_stun_server = optarg;
      break;
    case '6':
      g_enable_ipv6 = true;
      break;
    default:
      fprintf(stderr, "Usage: %s [-u signal_url] [-s stun_server] [-6]\n",
              argv[0]);
      return 1;
    }
  }

  printf("=== WebRTC PeerConnection Client ===\n");
  printf("  Signal URL:  %s\n", signal_url);
  printf("  STUN server: %s\n", g_stun_server);
  printf("  IPv6:        %s\n\n", g_enable_ipv6 ? "enabled" : "disabled");

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  /* Connect to signaling server */
  xWsConnectConf ws_conf = {};
  ws_conf.url            = signal_url;
  ws_conf.timeout_ms     = 5000;

  printf("[ws] connecting to %s ...\n", signal_url);

  xErrno err = xWsConnect(g_loop, &ws_conf, &ws_cbs, nullptr);
  if (err != xErrno_Ok) {
    fprintf(stderr, "xWsConnect failed: %d\n", err);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  /* Read stdin in a separate thread */
  std::thread reader(stdin_thread_func);

  /* Run the event loop */
  xEventLoopRun(g_loop);

  /* Cleanup */
  g_done = true;
  if (g_pc) {
    xPeerConnectionDestroy(g_pc);
    g_pc = nullptr;
  }

  if (reader.joinable()) {
    reader.join();
  }

  xEventLoopDestroy(g_loop);
  printf("[pc] bye!\n");
  return 0;
}
