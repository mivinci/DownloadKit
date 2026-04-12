/*
 * xfer_recv.c - P2P file transfer receiver (via signaling server)
 *
 * Connects to a signaling server with a code obtained from the sender,
 * performs SDP/ICE exchange automatically, and receives the file over
 * a P2P DataChannel.
 *
 * Usage:
 *   ./xfer_recv -c <code> [-d dest_dir] [-u ws://host:port/ws] [-s stun:port] [-6]
 *
 * Example:
 *   ./xfer_recv -c AB12CD
 *   ./xfer_recv -c AB12CD -d /tmp/received -u ws://192.168.1.100:8080/ws
 */

#include <xbase/event.h>
#include <xfer/xfer.h>

#include <signal.h> /* SIGINT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── State ─────────────────────────────────────────────── */

static xEventLoop g_loop;
static xTransfer  g_xfer;

/* ── Callbacks ─────────────────────────────────────────── */

static void on_state_change(xTransfer xfer, xTransferState state, void *ctx) {
  (void)xfer;
  (void)ctx;
  const char *s;
  switch (state) {
  case xTransferState_Idle:         s = "Idle";         break;
  case xTransferState_WaitingPeer:  s = "WaitingPeer";  break;
  case xTransferState_Connecting:   s = "Connecting";   break;
  case xTransferState_Transferring: s = "Transferring"; break;
  case xTransferState_Done:
    printf("\n[Recv] ✅ Transfer complete!\n");
    xEventLoopStop(g_loop);
    return;
  case xTransferState_Failed:
    printf("\n[Recv] ❌ Transfer failed.\n");
    xEventLoopStop(g_loop);
    return;
  default: s = "Unknown"; break;
  }
  printf("[Recv] State: %s\n", s);
}

static void on_progress(xTransfer xfer, uint64_t transferred,
                        uint64_t total, void *ctx) {
  (void)xfer;
  (void)ctx;
  printf("\r[Recv] Progress: %llu / %llu bytes (%.1f%%)",
         (unsigned long long)transferred, (unsigned long long)total,
         total > 0 ? 100.0 * transferred / total : 0.0);
  fflush(stdout);
}

static void on_file_meta(xTransfer xfer, const char *filename,
                         uint64_t filesize, void *ctx) {
  (void)xfer;
  (void)ctx;
  printf("[Recv] Incoming file: \"%s\" (%llu bytes)\n",
         filename, (unsigned long long)filesize);
}

static void on_error(xTransfer xfer, xErrno err, const char *msg, void *ctx) {
  (void)xfer;
  (void)err;
  (void)ctx;
  fprintf(stderr, "[Recv] Error: %s\n", msg);
}

static void on_sigint(int signo, void *arg) {
  (void)signo;
  xEventLoop loop = (xEventLoop)arg;
  printf("\n[Recv] Interrupted, shutting down...\n");
  if (g_xfer) xTransferCancel(g_xfer);
  xEventLoopStop(loop);
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  const char *code          = NULL;
  const char *dest_dir      = "/tmp/xfer_recv";
  const char *signal_url    = "ws://127.0.0.1:8080/ws";
  const char *stun_server   = NULL;
  bool        enable_ipv6   = false;

  int opt;
  while ((opt = getopt(argc, argv, "c:d:u:s:6")) != -1) {
    switch (opt) {
    case 'c': code        = optarg; break;
    case 'd': dest_dir    = optarg; break;
    case 'u': signal_url  = optarg; break;
    case 's': stun_server = optarg; break;
    case '6': enable_ipv6 = true;   break;
    default:
      fprintf(stderr,
              "Usage: %s -c <code> [-d dest_dir] [-u ws://host:port/ws] "
              "[-s stun:port] [-6]\n", argv[0]);
      return 1;
    }
  }

  if (!code) {
    fprintf(stderr, "Error: -c <code> is required\n");
    fprintf(stderr,
            "Usage: %s -c <code> [-d dest_dir] [-u ws://host:port/ws] "
            "[-s stun:port] [-6]\n", argv[0]);
    return 1;
  }

  printf("xfer Recv\n");
  printf("Code:    %s\n", code);
  printf("Dest:    %s\n", dest_dir);
  printf("Signal:  %s\n", signal_url);
  printf("STUN:    %s\n", stun_server ? stun_server : "(none)");
  printf("IPv6:    %s\n", enable_ipv6 ? "enabled" : "disabled");

  /* Ensure destination directory exists */
  mkdir(dest_dir, 0755);

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xEventLoopSignalWatch(g_loop, SIGINT, on_sigint, g_loop);

  xTransferConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.stun_server     = stun_server;
  conf.enable_ipv6     = enable_ipv6;
  conf.signal_server   = signal_url;
  conf.on_state_change = on_state_change;
  conf.on_progress     = on_progress;
  conf.on_file_meta    = on_file_meta;
  conf.on_error        = on_error;

  g_xfer = xTransferCreate(g_loop, &conf);
  if (!g_xfer) {
    fprintf(stderr, "Failed to create transfer\n");
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xErrno err = xTransferRecvFile(g_xfer, code, dest_dir);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to start receiving: %d\n", err);
    xTransferDestroy(g_xfer);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  printf("Joining session with code: %s\n", code);
  printf("Waiting for sender...\n\n");

  xEventLoopRun(g_loop);

  xTransferDestroy(g_xfer);
  xEventLoopDestroy(g_loop);
  return 0;
}
