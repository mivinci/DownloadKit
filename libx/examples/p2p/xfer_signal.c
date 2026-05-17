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

#include <x/base/event.h>
#include <x/fer/xfer_signal.h>

#include <signal.h> /* SIGINT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_sigint(int signo, void *arg) {
  (void)signo;
  xEventLoop loop = (xEventLoop)arg;
  printf("\nShutting down...\n");
  xEventLoopStop(loop);
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

  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xEventLoopSignalWatch(loop, SIGINT, on_sigint, loop);

  xSignalServerConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.host = host;
  conf.port = port;

  xSignalServer server = xSignalServerCreate(loop, &conf);
  if (!server) {
    fprintf(stderr, "Failed to create signaling server\n");
    xEventLoopDestroy(loop);
    return 1;
  }

  printf("Server started. Press Ctrl+C to stop.\n");
  xEventLoopRun(loop);

  xSignalServerDestroy(server);
  xEventLoopDestroy(loop);

  printf("Server stopped.\n");
  return 0;
}
