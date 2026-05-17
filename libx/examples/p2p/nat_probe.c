/*
 * nat_probe.c - NAT type detection demo
 *
 * Detects the local NAT type using two STUN servers and prints the
 * result. By default, Google's public STUN servers are used.
 *
 * Usage:
 *   ./nat_probe [-a stun1:port] [-b stun2:port] [-t timeout_ms]
 *
 * Example:
 *   ./nat_probe
 *   ./nat_probe -a stun1.l.google.com:19302 -b stun2.l.google.com:19302
 *   ./nat_probe -t 5000
 */

#include <x/base/event.h>
#include <x/p2p/nat_probe.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static xNatProbe  g_probe = NULL;
static xEventLoop g_loop  = NULL;

/* ── Callback ──────────────────────────────────────────── */

static void on_probe_done(const xNatProbeResult *result, void *arg) {
  (void)arg;
  g_probe = NULL; /* handle is auto-freed after callback */

  printf("\n");
  printf("═══════════════════════════════════════\n");
  printf("  NAT Type: %s\n", xNatTypeStr(result->type));

  if (result->type == xNatType_SymmetricSequential) {
    printf("  Port Delta: %d\n", result->port_delta);
  }

  printf("  Mapped Ports (phase1): %u, %u\n", result->mapped_ports[0], result->mapped_ports[1]);

  if (result->type == xNatType_SymmetricSequential || result->type == xNatType_SymmetricRandom) {
    printf("  Mapped Ports (phase2): %u, %u, %u\n", result->mapped_ports[2],
           result->mapped_ports[3], result->mapped_ports[4]);
  }

  printf("═══════════════════════════════════════\n");

  xEventLoopStop(g_loop);
}

/* ── Signal handler ────────────────────────────────────── */

static void on_sigint(int signo, void *arg) {
  (void)signo;
  (void)arg;
  printf("\nCancelling probe...\n");
  if (g_probe) {
    xNatProbeCancel(g_probe);
    g_probe = NULL;
  }
  xEventLoopStop(g_loop);
}

/* ── Helpers ───────────────────────────────────────────── */

/**
 * @brief Parse "host:port" string. Returns false on failure.
 */
static bool parse_host_port(const char *str, char *host, size_t host_len, uint16_t *port) {
  const char *colon = strrchr(str, ':');
  if (!colon || colon == str) return false;

  size_t hlen = (size_t)(colon - str);
  if (hlen >= host_len) return false;

  memcpy(host, str, hlen);
  host[hlen] = '\0';
  *port      = (uint16_t)atoi(colon + 1);
  return *port > 0;
}

/* ── Main ──────────────────────────────────────────────── */

static const char *DEFAULT_STUN1 = "stun1.l.google.com:19302";
static const char *DEFAULT_STUN2 = "stun2.l.google.com:19302";

int main(int argc, char *argv[]) {
  const char *stun1_str = DEFAULT_STUN1;
  const char *stun2_str = DEFAULT_STUN2;
  int         timeout   = 3000;

  int opt;
  while ((opt = getopt(argc, argv, "a:b:t:")) != -1) {
    switch (opt) {
    case 'a':
      stun1_str = optarg;
      break;
    case 'b':
      stun2_str = optarg;
      break;
    case 't':
      timeout = atoi(optarg);
      break;
    default:
      fprintf(stderr, "Usage: %s [-a stun1:port] [-b stun2:port] [-t timeout_ms]\n", argv[0]);
      return 1;
    }
  }

  /* Parse STUN server addresses */
  char     host1[256], host2[256];
  uint16_t port1, port2;

  if (!parse_host_port(stun1_str, host1, sizeof(host1), &port1)) {
    fprintf(stderr, "Invalid STUN server 1: %s\n", stun1_str);
    return 1;
  }
  if (!parse_host_port(stun2_str, host2, sizeof(host2), &port2)) {
    fprintf(stderr, "Invalid STUN server 2: %s\n", stun2_str);
    return 1;
  }

  printf("=== NAT Probe Demo ===\n");
  printf("STUN server 1: %s:%u\n", host1, port1);
  printf("STUN server 2: %s:%u\n", host2, port2);
  printf("Timeout:       %d ms\n\n", timeout);

  g_loop = xEventLoopCreate();
  if (!g_loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  xEventLoopSignalWatch(g_loop, SIGINT, on_sigint, NULL);

  printf("Probing NAT type...\n");

  g_probe = xNatProbeStart(g_loop, host1, port1, host2, port2, timeout, on_probe_done, NULL);
  if (!g_probe) {
    fprintf(stderr, "Failed to start NAT probe\n");
    xEventLoopDestroy(g_loop);
    return 1;
  }

  xEventLoopRun(g_loop);

  xEventLoopDestroy(g_loop);
  return 0;
}
