/*
 * xfer_signal.c - Standalone signaling server for P2P file transfer
 *
 * Runs a WebSocket-based signaling server that pairs sender and
 * receiver peers and relays SDP/ICE messages between them.
 *
 * Usage:
 *   ./xfer_signal [-p port] [-h host]
 *
 * Example:
 *   ./xfer_signal -p 8080
 *   ./xfer_signal -h 0.0.0.0 -p 9000
 */

#include <xbase/event.h>
#include <xfer/xfer_signal.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xEventLoop    g_loop;
static xSignalServer g_server;

static void on_sigint(int sig) {
  (void)sig;
  printf("\nShutting down...\n");
  if (g_loop) {
    xEventLoopStop(g_loop);
  }
}

int main(int argc, char *argv[]) {
  const char *host = "0.0.0.0";
  uint16_t    port = 8080;

  int opt;
  while ((opt = getopt(argc, argv, "h:p:")) != -1) {
    switch (opt) {
    case 'h':
      host = optarg;
      break;
    case 'p':
      port = (uint16_t)atoi(optarg);
      break;
    default:
      fprintf(stderr, "Usage: %s [-h host] [-p port]\n", argv[0]);
      return 1;
    }
  }

  printf("xfer Signaling Server\n");
  printf("Listening on %s:%u\n", host, port);

  signal(SIGINT, on_sigint);

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xSignalServerConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.host = host;
  conf.port = port;

  g_server = xSignalServerCreate(g_loop, &conf);
  if (!g_server) {
    fprintf(stderr, "Failed to create signaling server\n");
    xEventLoopDestroy(g_loop);
    return 1;
  }

  printf("Server started. Press Ctrl+C to stop.\n");
  xEventLoopRun(g_loop);

  xSignalServerDestroy(g_server);
  xEventLoopDestroy(g_loop);

  printf("Server stopped.\n");
  return 0;
}
