/*
 * pc_server.cpp - WebSocket signaling server for WebRTC demo
 *
 * Relays SDP offers/answers and ICE candidates between two peers:
 *   - A browser running pc_client.html
 *   - A native C client (pc_client)
 *
 * Both peers connect via WebSocket to ws://localhost:<port>/signal.
 * Messages are plain-text JSON:
 *   {"type":"offer","sdp":"..."}
 *   {"type":"answer","sdp":"..."}
 *   {"type":"candidate","candidate":"..."}
 *
 * The server simply forwards each message to the other peer.
 * The HTML page is served at http://localhost:<port>/.
 *
 * Usage:
 *   ./pc_server [port]
 *
 * Test flow:
 *   1. ./pc_server 8080
 *   2. Open http://localhost:8080/ in a browser
 *   3. ./pc_client ws://localhost:8080/signal
 *   4. The browser and C client exchange SDP and establish a DataChannel
 */

#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xhttp/server.h>
#include <xhttp/ws.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libgen.h>     /* dirname() */
#include <fcntl.h>      /* open() */
#include <sys/mman.h>   /* mmap(), munmap() */
#include <sys/stat.h>    /* fstat() */
#include <unistd.h>     /* close() */

/* ── Peer tracking ─────────────────────────────────────── */

/* Simple two-slot peer table. The signaling server only
 * supports exactly two peers at a time. */
static xWsConn g_peers[2] = {nullptr, nullptr};

static int peer_index(xWsConn conn) {
  for (int i = 0; i < 2; i++) {
    if (g_peers[i] == conn) return i;
  }
  return -1;
}

static xWsConn peer_other(xWsConn conn) {
  if (g_peers[0] == conn) return g_peers[1];
  if (g_peers[1] == conn) return g_peers[0];
  return nullptr;
}

static int peer_add(xWsConn conn) {
  for (int i = 0; i < 2; i++) {
    if (g_peers[i] == nullptr) {
      g_peers[i] = conn;
      return i;
    }
  }
  return -1; /* full */
}

static void peer_remove(xWsConn conn) {
  for (int i = 0; i < 2; i++) {
    if (g_peers[i] == conn) {
      g_peers[i] = nullptr;
      return;
    }
  }
}

/* ── WebSocket callbacks ───────────────────────────────── */

static void on_open(xWsConn conn, void *arg) {
  (void)arg;
  int slot = peer_add(conn);
  if (slot < 0) {
    printf("[signal] rejected connection (already 2 peers)\n");
    xWsClose(conn, 1013); /* Try Again Later */
    return;
  }
  printf("[signal] peer %d connected (%p)\n", slot, (void *)conn);

  /* Notify the peer how many are connected */
  int count = (g_peers[0] ? 1 : 0) + (g_peers[1] ? 1 : 0);
  if (count == 2) {
    printf("[signal] both peers connected, ready for signaling\n");
  }
}

static void on_message(xWsConn conn, xWsOpcode opcode,
                       const void *payload, size_t len,
                       void *arg) {
  (void)arg;
  int idx = peer_index(conn);
  printf("[signal] peer %d sent %zu bytes\n", idx, len);

  /* Forward to the other peer */
  xWsConn other = peer_other(conn);
  if (other) {
    xWsSend(other, opcode, payload, len);
    printf("[signal] forwarded to peer %d\n",
           peer_index(other));
  } else {
    printf("[signal] no other peer to forward to\n");
  }
}

static void on_close(xWsConn conn, uint16_t code,
                     const char *reason, size_t len,
                     void *arg) {
  (void)arg;
  int idx = peer_index(conn);
  printf("[signal] peer %d disconnected (code=%u reason=%.*s)\n",
         idx, code, (int)len, reason ? reason : "");
  peer_remove(conn);
}

static const xWsCallbacks ws_cbs = {
  .on_open    = on_open,
  .on_message = on_message,
  .on_close   = on_close,
};

/* ── HTML page ─────────────────────────────────────────── */

/* Resolve pc_client.html relative to this source file so the
 * server works regardless of the current working directory. */
static const char *html_path_relative_to_source() {
  static char buf[4096];
  snprintf(buf, sizeof(buf), "%s", __FILE__);
  char *dir = dirname(buf);
  snprintf(buf, sizeof(buf), "%s/pc_client.html", dir);
  return buf;
}

static char  *g_html      = nullptr;
static size_t g_html_len  = 0;

static void load_html(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("[signal] warning: could not open %s\n", path);
    return;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0) {
    close(fd);
    return;
  }
  void *addr = mmap(nullptr, (size_t)st.st_size, PROT_READ,
                    MAP_PRIVATE, fd, 0);
  close(fd);
  if (addr == MAP_FAILED) {
    printf("[signal] warning: mmap failed for %s\n", path);
    return;
  }
  g_html     = (char *)addr;
  g_html_len = (size_t)st.st_size;
  printf("[signal] loaded HTML client (%zu bytes, mmap)\n", g_html_len);
}

static const char *FALLBACK_HTML =
  "<!DOCTYPE html><html><body>"
  "<h2>pc_client.html not found</h2>"
  "<p>Place pc_client.html next to the server binary.</p>"
  "</body></html>";

/* ── HTTP handlers ─────────────────────────────────────── */

static void index_handler(xHttpResponseWriter w,
                          const xHttpRequest *req,
                          void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type",
                         "text/html; charset=utf-8");
  if (g_html) {
    xHttpResponseSend(w, g_html, g_html_len);
  } else {
    xHttpResponseSend(w, FALLBACK_HTML, strlen(FALLBACK_HTML));
  }
}

static void ws_handler(xHttpResponseWriter w,
                       const xHttpRequest *req,
                       void *arg) {
  (void)arg;
  xErrno err = xWsUpgrade(w, req, &ws_cbs, nullptr);
  if (err != xErrno_Ok) {
    printf("[signal] WebSocket upgrade failed: %d\n", err);
  }
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  uint16_t port = 8080;
  if (argc > 1) {
    port = (uint16_t)atoi(argv[1]);
  }

  /* Load pc_client.html from the same directory as this source file */
  load_html(html_path_relative_to_source());

  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xHttpServer server = xHttpServerCreate(loop);
  if (!server) {
    fprintf(stderr, "Failed to create HTTP server\n");
    xEventLoopDestroy(loop);
    return 1;
  }

  // Set idle timeout to 5 minutes
  xHttpServerSetIdleTimeout(server, 300000);

  xHttpServerRoute(server, "GET /", index_handler, nullptr);
  xHttpServerRoute(server, "GET /signal", ws_handler, nullptr);

  xErrno err = xHttpServerListen(server, "0.0.0.0", port);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to listen on port %u: %d\n",
            port, err);
    xHttpServerDestroy(server);
    xEventLoopDestroy(loop);
    if (g_html) munmap(g_html, g_html_len);
    return 1;
  }

  printf("=== WebRTC Signaling Server ===\n");
  printf("  HTML client: http://localhost:%u/\n", port);
  printf("  WebSocket:   ws://localhost:%u/signal\n", port);
  printf("Press Ctrl-C to stop.\n\n");

  xEventLoopRun(loop);

  xHttpServerDestroy(server);
  xEventLoopDestroy(loop);
  if (g_html) munmap(g_html, g_html_len);
  return 0;
}
