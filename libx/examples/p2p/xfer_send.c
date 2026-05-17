/*
 * xfer_send.c - P2P file transfer sender (via signaling server)
 *
 * Connects to a signaling server, obtains a short code, and waits
 * for a receiver to join. Once paired, SDP/ICE exchange happens
 * automatically through the signaling server, and the file is
 * transferred over a P2P DataChannel.
 *
 * Usage:
 *   ./xfer_send -f <file> [-u ws://host:port/ws] [-s stun:port]
 *              [-t turn:port] [-U user] [-P pass] [-6]
 *
 * Example:
 *   ./xfer_send -f myfile.bin
 *   ./xfer_send -f myfile.bin -u ws://192.168.1.100:8080/ws
 *   ./xfer_send -f myfile.bin -t openrelay.metered.ca:443 -U openrelayproject -P openrelayproject
 */

#include <x/base/event.h>
#include <x/base/speed_tracker.h>
#include <x/fer/xfer.h>

#include <signal.h> /* SIGINT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── State ─────────────────────────────────────────────── */

static xEventLoop    g_loop;
static xTransfer     g_xfer;
static xSpeedTracker spd = XSPEED_TRACKER_INIT(0.3);

/* ── Callbacks ─────────────────────────────────────────── */

static void on_state_change(xTransfer xfer, xTransferState state, void *ctx) {
  (void)xfer;
  (void)ctx;
  const char *s;
  switch (state) {
  case xTransferState_Idle:
    s = "Idle";
    break;
  case xTransferState_WaitingPeer:
    s = "WaitingPeer";
    break;
  case xTransferState_Connecting:
    s = "Connecting";
    break;
  case xTransferState_Transferring:
    s = "Transferring";
    break;
  case xTransferState_Done:
    printf("\n[Send] ✅ Transfer complete!\n");
    xEventLoopStop(g_loop);
    return;
  case xTransferState_Failed:
    printf("\n[Send] ❌ Transfer failed.\n");
    xEventLoopStop(g_loop);
    return;
  default:
    s = "Unknown";
    break;
  }
  printf("[Send] State: %s\n", s);
}

static void on_progress(xTransfer xfer, uint64_t transferred, uint64_t total, void *ctx) {
  (void)xfer;
  (void)ctx;
  char speed_buf[32];

  xSpeedTrackerUpdate(&spd, transferred);
  xSpeedTrackerFormat(&spd, speed_buf, sizeof(speed_buf));

  printf("\r[Send] Progress: %llu / %llu bytes (%.1f%%)%s   ", (unsigned long long)transferred,
         (unsigned long long)total, total > 0 ? 100.0 * transferred / total : 0.0, speed_buf);
  fflush(stdout);
}

static void on_code(xTransfer xfer, const char *code, void *ctx) {
  (void)xfer;
  (void)ctx;
  printf("\n═══════════════════════════\n");
  printf("  Code: %s\n", code);
  printf("═══════════════════════════\n");
  printf("\nShare this code with the receiver.\n");
  printf("Waiting for peer to join...\n\n");
}

static void on_error(xTransfer xfer, xErrno err, const char *msg, void *ctx) {
  (void)xfer;
  (void)err;
  (void)ctx;
  fprintf(stderr, "[Send] Error: %s\n", msg);
}

static void on_sigint(int signo, void *arg) {
  (void)signo;
  xEventLoop loop = (xEventLoop)arg;
  printf("\n[Send] Interrupted, shutting down...\n");
  if (g_xfer) xTransferCancel(g_xfer);
  xEventLoopStop(loop);
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  const char *filepath      = NULL;
  const char *signal_url    = "ws://127.0.0.1:8080/ws";
  const char *stun_server   = "stun1.l.google.com:19302,stun.cloudflare.com:3478";
  const char *turn_server   = NULL;
  const char *turn_username = NULL;
  const char *turn_password = NULL;
  bool        enable_ipv6   = false;

  int opt;
  while ((opt = getopt(argc, argv, "f:u:s:t:U:P:6")) != -1) {
    switch (opt) {
    case 'f':
      filepath = optarg;
      break;
    case 'u':
      signal_url = optarg;
      break;
    case 's':
      stun_server = optarg;
      break;
    case 't':
      turn_server = optarg;
      break;
    case 'U':
      turn_username = optarg;
      break;
    case 'P':
      turn_password = optarg;
      break;
    case '6':
      enable_ipv6 = true;
      break;
    default:
      fprintf(stderr,
              "Usage: %s -f <file> [-u ws://host:port/ws] "
              "[-s stun:port] [-t turn:port] [-U user] [-P pass] [-6]\n",
              argv[0]);
      return 1;
    }
  }

  if (!filepath) {
    fprintf(stderr, "Error: -f <file> is required\n");
    fprintf(stderr,
            "Usage: %s -f <file> [-u ws://host:port/ws] "
            "[-s stun:port] [-t turn:port] [-U user] [-P pass] [-6]\n",
            argv[0]);
    return 1;
  }

  printf("xfer Send\n");
  printf("File:    %s\n", filepath);
  printf("Signal:  %s\n", signal_url);
  printf("STUN:    %s\n", stun_server);
  printf("TURN:    %s\n", turn_server ? turn_server : "(none)");
  printf("IPv6:    %s\n", enable_ipv6 ? "enabled" : "disabled");

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xEventLoopSignalWatch(g_loop, SIGINT, on_sigint, g_loop);

  xTransferConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.stun_server     = stun_server;
  conf.turn_server     = turn_server;
  conf.turn_username   = turn_username;
  conf.turn_password   = turn_password;
  conf.enable_ipv6     = enable_ipv6;
  conf.signal_server   = signal_url;
  conf.on_state_change = on_state_change;
  conf.on_progress     = on_progress;
  conf.on_code         = on_code;
  conf.on_error        = on_error;

  g_xfer = xTransferCreate(g_loop, &conf);
  if (!g_xfer) {
    fprintf(stderr, "Failed to create transfer\n");
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xErrno err = xTransferSendFile(g_xfer, filepath);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to start sending: %d\n", err);
    xTransferDestroy(g_xfer);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xEventLoopRun(g_loop);

  xTransferDestroy(g_xfer);
  xEventLoopDestroy(g_loop);
  return 0;
}
