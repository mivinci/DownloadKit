/*
 * ws_echo_server.cpp - WebSocket echo server demo
 *
 * Usage:
 *   ./ws_echo_server [port]
 *
 * Open ws_client.html in a browser and connect to ws://localhost:<port>/ws.
 * Messages sent from the browser are echoed back by the server.
 * The server also serves the HTML client at http://localhost:<port>/.
 */

#include <x/base/backtrace.h>
#include <x/base/event.h>
#include <x/http/server.h>
#include <x/http/ws.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ── WebSocket callbacks ───────────────────────────────── */

static void on_open(xWsConn conn, void *arg) {
  (void)arg;
  printf("[ws] connection opened: %p\n", (void *)conn);

  const char *greeting = "Welcome to the echo server!";
  xWsSend(conn, xWsOpcode_Text, greeting, strlen(greeting));
}

static void on_message(xWsConn conn, xWsOpcode opcode, const void *payload, size_t len, void *arg) {
  (void)arg;

  if (opcode == xWsOpcode_Text) {
    printf("[ws] text (%zu bytes): %.*s\n", len, (int)len, (const char *)payload);
  } else {
    printf("[ws] binary (%zu bytes)\n", len);
  }

  /* Echo the message back */
  xWsSend(conn, opcode, payload, len);
}

static void on_close(xWsConn conn, uint16_t code, const char *reason, size_t len, void *arg) {
  (void)arg;
  printf("[ws] connection closed: %p code=%u reason=%.*s\n", (void *)conn, code, (int)len,
         reason ? reason : "");
}

static const xWsCallbacks ws_cbs = {
  .on_open    = on_open,
  .on_message = on_message,
  .on_close   = on_close,
};

/* ── HTTP handlers ─────────────────────────────────────── */

/* Minimal inline HTML client (fallback if ws_client.html
   is not served by a separate file server). */
static const char *INDEX_HTML = "<!DOCTYPE html>\n"
                                "<html><head><meta charset=\"utf-8\">\n"
                                "<title>WebSocket Echo</title></head>\n"
                                "<body>\n"
                                "<h2>WebSocket Echo Client</h2>\n"
                                "<p>Status: <span id=\"st\">disconnected</span></p>\n"
                                "<input id=\"msg\" placeholder=\"type a message...\">\n"
                                "<button id=\"btn\">Send</button>\n"
                                "<pre id=\"log\"></pre>\n"
                                "<script>\n"
                                "const log=document.getElementById('log');\n"
                                "const st=document.getElementById('st');\n"
                                "const msg=document.getElementById('msg');\n"
                                "const btn=document.getElementById('btn');\n"
                                "let ws;\n"
                                "function connect(){\n"
                                "  ws=new WebSocket("
                                "'ws://'+location.host+'/ws');\n"
                                "  ws.onopen=()=>{st.textContent='connected';\n"
                                "    log.textContent+='[open]\\n';};\n"
                                "  ws.onmessage=(e)=>{\n"
                                "    log.textContent+='< '+e.data+'\\n';};\n"
                                "  ws.onclose=(e)=>{\n"
                                "    st.textContent='closed ('+e.code+')';\n"
                                "    log.textContent+='[close]\\n';};\n"
                                "  ws.onerror=()=>{\n"
                                "    log.textContent+='[error]\\n';};\n"
                                "}\n"
                                "btn.onclick=()=>{\n"
                                "  if(ws&&ws.readyState===1){\n"
                                "    log.textContent+='> '+msg.value+'\\n';\n"
                                "    ws.send(msg.value);msg.value='';}\n"
                                "};\n"
                                "msg.addEventListener('keydown',(e)=>{\n"
                                "  if(e.key==='Enter')btn.click();});\n"
                                "connect();\n"
                                "</script></body></html>\n";

static void index_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseSetStatus(w, 200);
  xHttpResponseSetHeader(w, "Content-Type", "text/html; charset=utf-8");
  xHttpResponseSend(w, INDEX_HTML, strlen(INDEX_HTML));
}

static void ws_handler(xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
  (void)arg;
  xErrno err = xWsUpgrade(w, req, &ws_cbs, NULL);
  if (err != xErrno_Ok) {
    /* xWsUpgrade already sent an error response */
    printf("[ws] upgrade failed: %d\n", err);
  }
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  /* Install crash signal handlers for backtrace */
  xPrintBacktraceOnCrash();

  uint16_t port = 8080;
  if (argc > 1) {
    port = (uint16_t)atoi(argv[1]);
  }

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

  xHttpServerRoute(server, "GET /", index_handler, NULL);
  xHttpServerRoute(server, "GET /ws", ws_handler, NULL);

  xErrno err = xHttpServerListen(server, "0.0.0.0", port);
  if (err != xErrno_Ok) {
    fprintf(stderr, "Failed to listen on port %u: %d\n", port, err);
    xHttpServerDestroy(server);
    xEventLoopDestroy(loop);
    return 1;
  }

  printf("WebSocket echo server listening on "
         "http://0.0.0.0:%u\n",
         port);
  printf("  HTML client: http://localhost:%u/\n", port);
  printf("  WebSocket:   ws://localhost:%u/ws\n", port);
  printf("Press Ctrl-C to stop.\n\n");

  xEventLoopRun(loop);

  xHttpServerDestroy(server);
  xEventLoopDestroy(loop);
  return 0;
}
