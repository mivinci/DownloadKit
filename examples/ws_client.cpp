/*
 * ws_client.cpp - WebSocket client demo
 *
 * Usage:
 *   ./ws_client [url]
 *
 * Connects to a WebSocket server (default: ws://127.0.0.1:9000)
 * and enters an interactive loop: type a line and press Enter to
 * send it as a text message. Received messages are printed to
 * stdout. Type "quit" or press Ctrl-C to disconnect.
 *
 * Pair with ws_echo_server.py for a quick test:
 *   python3 ws_echo_server.py &
 *   ./ws_client ws://127.0.0.1:9000
 */

#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xhttp/ws.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

/* ── Global state ──────────────────────────────────────── */

static xEventLoop   g_loop = nullptr;
static xWsConn      g_conn = nullptr;
static std::atomic<bool> g_connected{false};
static std::atomic<bool> g_done{false};

/* ── WebSocket callbacks ───────────────────────────────── */

static void on_open(xWsConn conn, void *arg) {
  (void)arg;
  g_conn = conn;
  g_connected = true;
  printf("[ws] connected!\n");
  printf("[ws] type a message and press Enter "
         "(\"quit\" to close)\n");
}

static void on_message(xWsConn conn, xWsOpcode opcode,
                       const void *payload, size_t len,
                       void *arg) {
  (void)conn;
  (void)arg;

  if (opcode == xWsOpcode_Text) {
    printf("< %.*s\n", (int)len, (const char *)payload);
  } else {
    printf("< [binary %zu bytes]\n", len);
  }
}

static void on_close(xWsConn conn, uint16_t code,
                     const char *reason, size_t len,
                     void *arg) {
  (void)conn;
  (void)arg;
  printf("[ws] closed (code=%u", code);
  if (reason && len > 0) {
    printf(", reason=%.*s", (int)len, reason);
  }
  printf(")\n");

  g_connected = false;
  g_conn = nullptr;
  g_done = true;

  /* Stop the event loop so the program exits */
  xEventLoopStop(g_loop);
}

static const xWsCallbacks ws_cbs = {
  .on_open    = on_open,
  .on_message = on_message,
  .on_close   = on_close,
};

/* ── Stdin reader thread ───────────────────────────────── */

static void stdin_thread_func() {
  char line[4096];

  while (!g_done) {
    if (!fgets(line, sizeof(line), stdin)) {
      break; /* EOF or error */
    }

    /* Strip trailing newline */
    size_t n = strlen(line);
    while (n > 0 &&
           (line[n - 1] == '\n' || line[n - 1] == '\r')) {
      line[--n] = '\0';
    }
    if (n == 0) continue;

    if (!g_connected || !g_conn) {
      printf("[ws] not connected yet\n");
      continue;
    }

    if (strcmp(line, "quit") == 0) {
      printf("[ws] closing...\n");
      xWsClose(g_conn, 1000);
      break;
    }

    xErrno err = xWsSend(g_conn, xWsOpcode_Text, line, n);
    if (err != xErrno_Ok) {
      printf("[ws] send failed: %d\n", err);
    }
  }
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  const char *url = "ws://127.0.0.1:9000";
  if (argc > 1) {
    url = argv[1];
  }

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xWsConnectConf conf = {};
  conf.url        = url;
  conf.timeout_ms = 5000;

  printf("[ws] connecting to %s ...\n", url);

  xErrno err = xWsConnect(g_loop, &conf, &ws_cbs, nullptr);
  if (err != xErrno_Ok) {
    fprintf(stderr, "xWsConnect failed: %d\n", err);
    xEventLoopDestroy(g_loop);
    return 1;
  }

  /* Read stdin in a separate thread so the event loop
   * is free to process I/O. */
  std::thread reader(stdin_thread_func);

  /* Run the event loop (blocks until xEventLoopStop) */
  xEventLoopRun(g_loop);

  g_done = true;
  if (reader.joinable()) {
    reader.join();
  }

  xEventLoopDestroy(g_loop);
  printf("[ws] bye!\n");
  return 0;
}
