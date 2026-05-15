/*
 * xfer_echo.c - P2P file transfer loopback demo
 *
 * Creates two xTransfer instances (sender + receiver) in the same
 * process, exchanges SDP directly in memory (no signaling server),
 * and transfers a file via the xfer high-level API.
 *
 * This demonstrates:
 *   - xTransfer API for sending and receiving files
 *   - SDP exchange via xTransferCreateOffer / CreateAnswer
 *   - ICE gathering via xTransferGatherCandidates
 *   - Resume (断点续传): with -r flag, cancels at 50% then resumes
 *
 * Usage:
 *   ./xfer_echo [-f file] [-d dest_dir] [-r] [-s stun:port] [-6]
 *
 * Example:
 *   ./xfer_echo                     # generate 1MB temp file, transfer
 *   ./xfer_echo -f myfile.bin       # transfer an existing file
 *   ./xfer_echo -r                  # test resume (cancel at 50%)
 */

#include <xbase/event.h>
#include <xfer/xfer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Constants ─────────────────────────────────────────── */

#define DEFAULT_FILE_SIZE (1024 * 1024) /* 1 MB */

/* ── Globals ───────────────────────────────────────────── */

static xEventLoop g_loop;
static xTransfer  g_sender;
static xTransfer  g_receiver;

static bool g_sender_gathering_done   = false;
static bool g_receiver_gathering_done = false;

static const char *g_stun_server = NULL;
static bool        g_enable_ipv6 = false;
static bool        g_resume_test = false;

/* File to send */
static const char *g_send_filepath = NULL;
static char        g_temp_filepath[512];
static bool        g_temp_file_created = false;

/* Destination directory */
static const char *g_dest_dir = "/tmp/xfer_echo_recv";

/* Resume test state */
static bool g_phase1_done = false;
static int  g_phase       = 1;

/* Track sender progress for resume-test cancellation */
static uint64_t g_sender_total    = 0;
static uint64_t g_sender_progress = 0;

/* Track both sides done for clean shutdown */
static bool g_sender_done   = false;
static bool g_receiver_done = false;

/* ── Forward declarations ──────────────────────────────── */

static void exchange_sdp(void);
static void start_transfer(void);

/* ── Generate a temporary test file ────────────────────── */

static int generate_temp_file(const char *path, size_t size) {
  FILE *fp = fopen(path, "wb");
  if (!fp) return -1;
  srand((unsigned)time(NULL));
  uint8_t buf[4096];
  size_t  remaining = size;
  while (remaining > 0) {
    size_t n = remaining < sizeof(buf) ? remaining : sizeof(buf);
    for (size_t i = 0; i < n; i++)
      buf[i] = (uint8_t)(rand() & 0xFF);
    fwrite(buf, 1, n, fp);
    remaining -= n;
  }
  fclose(fp);
  return 0;
}

/* ── File comparison ───────────────────────────────────── */

static bool files_equal(const char *path_a, const char *path_b) {
  FILE *fa = fopen(path_a, "rb");
  FILE *fb = fopen(path_b, "rb");
  if (!fa || !fb) {
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return false;
  }
  bool    equal = true;
  uint8_t ba[4096], bb[4096];
  while (1) {
    size_t na = fread(ba, 1, sizeof(ba), fa);
    size_t nb = fread(bb, 1, sizeof(bb), fb);
    if (na != nb) {
      equal = false;
      break;
    }
    if (na == 0) break;
    if (memcmp(ba, bb, na) != 0) {
      equal = false;
      break;
    }
  }
  fclose(fa);
  fclose(fb);
  return equal;
}

/* ── Sender callbacks ──────────────────────────────────── */

static void sender_on_state_change(xTransfer xfer, xTransferState state,
                                   void *ctx) {
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
    s = "Done";
    g_sender_done = true;
    if (g_receiver_done) {
      xEventLoopStop(g_loop);
    }
    break;
  case xTransferState_Failed:
    s = "Failed";
    break;
  default:
    s = "Unknown";
    break;
  }
  printf("[Sender] State: %s\n", s);
}

static void sender_on_progress(xTransfer xfer, uint64_t transferred,
                               uint64_t total, void *ctx) {
  (void)xfer;
  (void)ctx;
  g_sender_total    = total;
  g_sender_progress = transferred;

  printf("\r[Sender] Progress: %llu / %llu bytes (%.1f%%)",
         (unsigned long long)transferred, (unsigned long long)total,
         total > 0 ? 100.0 * transferred / total : 0.0);
  fflush(stdout);

  /* Resume test: cancel at ~50% in phase 1 */
  if (g_resume_test && g_phase == 1 && transferred >= total / 2) {
    printf("\n[Sender] Phase 1: cancelling at 50%%\n");
    g_phase1_done = true;
    xEventLoopStop(g_loop);
  }
}

static void sender_on_error(xTransfer xfer, xErrno err, const char *msg,
                            void *ctx) {
  (void)xfer;
  (void)err;
  (void)ctx;
  fprintf(stderr, "[Sender] Error: %s\n", msg);
}

static void sender_on_ice_candidate(xTransfer xfer, const char *candidate,
                                    void *ctx) {
  (void)xfer;
  (void)ctx;
  if (candidate) {
    printf("[Sender] ICE candidate: %s\n", candidate);
  } else {
    printf("[Sender] ICE gathering done\n");
    g_sender_gathering_done = true;
    if (g_sender_gathering_done && g_receiver_gathering_done) {
      exchange_sdp();
    }
  }
}

/* ── Receiver callbacks ────────────────────────────────── */

static void receiver_on_state_change(xTransfer xfer, xTransferState state,
                                     void *ctx) {
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
  case xTransferState_Done: {
    printf("[Receiver] State: Done\n");

    /* Verify transferred file */
    const char *src = g_send_filepath ? g_send_filepath : g_temp_filepath;
    char        final_path[1024];
    /* The filename is the basename of the source file */
    const char *name = strrchr(src, '/');
    name             = name ? name + 1 : src;
    snprintf(final_path, sizeof(final_path), "%s/%s", g_dest_dir, name);

    if (files_equal(src, final_path)) {
      printf("\n✅ File transfer successful! Files match.\n");
    } else {
      printf("\n❌ File mismatch! Transfer failed.\n");
    }

    g_receiver_done = true;
    if (g_sender_done) {
      xEventLoopStop(g_loop);
    }
    return;
  }
  case xTransferState_Failed:
    s = "Failed";
    break;
  default:
    s = "Unknown";
    break;
  }
  printf("[Receiver] State: %s\n", s);
}

static void receiver_on_progress(xTransfer xfer, uint64_t transferred,
                                 uint64_t total, void *ctx) {
  (void)xfer;
  (void)ctx;
  printf("\r[Receiver] Progress: %llu / %llu bytes (%.1f%%)",
         (unsigned long long)transferred, (unsigned long long)total,
         total > 0 ? 100.0 * transferred / total : 0.0);
  fflush(stdout);
}

static void receiver_on_file_meta(xTransfer xfer, const char *filename,
                                  uint64_t filesize, void *ctx) {
  (void)xfer;
  (void)ctx;
  printf("[Receiver] FILE_META: name=\"%s\" size=%llu\n", filename,
         (unsigned long long)filesize);
}

static void receiver_on_error(xTransfer xfer, xErrno err, const char *msg,
                              void *ctx) {
  (void)xfer;
  (void)err;
  (void)ctx;
  fprintf(stderr, "[Receiver] Error: %s\n", msg);
}

static void receiver_on_ice_candidate(xTransfer xfer, const char *candidate,
                                      void *ctx) {
  (void)xfer;
  (void)ctx;
  if (candidate) {
    printf("[Receiver] ICE candidate: %s\n", candidate);
  } else {
    printf("[Receiver] ICE gathering done\n");
    g_receiver_gathering_done = true;
    if (g_sender_gathering_done && g_receiver_gathering_done) {
      exchange_sdp();
    }
  }
}

/* ── SDP Exchange ──────────────────────────────────────── */

static void exchange_sdp(void) {
  printf("\n── Exchanging SDP ──\n\n");

  char *offer = xTransferCreateOffer(g_sender);
  if (!offer) {
    fprintf(stderr, "Failed to create offer\n");
    return;
  }

  xTransferSetLocalDescription(g_sender, offer);
  xTransferSetRemoteDescription(g_receiver, offer);

  char *answer = xTransferCreateAnswer(g_receiver);
  if (!answer) {
    fprintf(stderr, "Failed to create answer\n");
    free(offer);
    return;
  }

  xTransferSetLocalDescription(g_receiver, answer);
  xTransferSetRemoteDescription(g_sender, answer);

  free(offer);
  free(answer);
}

/* ── Start a transfer round ────────────────────────────── */

static void start_transfer(void) {
  printf("\n══ Phase %d %s ══\n\n", g_phase,
         (g_phase == 1 && g_resume_test) ? "(will cancel at 50%)"
         : (g_phase == 2)                ? "(resume)"
                                         : "");

  const char *path = g_send_filepath ? g_send_filepath : g_temp_filepath;

  /* Create sender */
  xTransferConf sender_conf;
  memset(&sender_conf, 0, sizeof(sender_conf));
  sender_conf.stun_server      = g_stun_server;
  sender_conf.enable_ipv6      = g_enable_ipv6;
  sender_conf.on_state_change  = sender_on_state_change;
  sender_conf.on_progress      = sender_on_progress;
  sender_conf.on_error         = sender_on_error;
  sender_conf.on_ice_candidate = sender_on_ice_candidate;

  g_sender = xTransferCreate(g_loop, &sender_conf);
  xTransferSendFile(g_sender, path);

  /* Create receiver */
  xTransferConf receiver_conf;
  memset(&receiver_conf, 0, sizeof(receiver_conf));
  receiver_conf.stun_server      = g_stun_server;
  receiver_conf.enable_ipv6      = g_enable_ipv6;
  receiver_conf.on_state_change  = receiver_on_state_change;
  receiver_conf.on_progress      = receiver_on_progress;
  receiver_conf.on_file_meta     = receiver_on_file_meta;
  receiver_conf.on_error         = receiver_on_error;
  receiver_conf.on_ice_candidate = receiver_on_ice_candidate;

  g_receiver = xTransferCreate(g_loop, &receiver_conf);
  xTransferRecvFile(g_receiver, "loopback", g_dest_dir);

  /* Start ICE gathering on both sides */
  xTransferGatherCandidates(g_sender);
  xTransferGatherCandidates(g_receiver);
}

/* ── Cleanup a transfer round ──────────────────────────── */

static void cleanup_round(void) {
  if (g_sender) {
    xTransferDestroy(g_sender);
    g_sender = NULL;
  }
  if (g_receiver) {
    xTransferDestroy(g_receiver);
    g_receiver = NULL;
  }
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
  int opt;
  while ((opt = getopt(argc, argv, "f:d:rs:6")) != -1) {
    switch (opt) {
    case 'f':
      g_send_filepath = optarg;
      break;
    case 'd':
      g_dest_dir = optarg;
      break;
    case 'r':
      g_resume_test = true;
      break;
    case 's':
      g_stun_server = optarg;
      break;
    case '6':
      g_enable_ipv6 = true;
      break;
    default:
      fprintf(stderr,
              "Usage: %s [-f file] [-d dest_dir] [-r] [-s stun:port] [-6]\n",
              argv[0]);
      return 1;
    }
  }

  printf("xfer Echo Demo\n");
  printf("STUN:    %s\n", g_stun_server ? g_stun_server : "(none)");
  printf("IPv6:    %s\n", g_enable_ipv6 ? "enabled" : "disabled");
  printf("Resume:  %s\n", g_resume_test ? "enabled" : "disabled");

  /* Create destination directory */
  mkdir(g_dest_dir, 0755);

  /* Generate temp file if none specified */
  if (!g_send_filepath) {
    snprintf(g_temp_filepath, sizeof(g_temp_filepath),
             "/tmp/xfer_echo_test_%d.bin", (int)getpid());
    printf("Generating temp file: %s (%d bytes)\n", g_temp_filepath,
           DEFAULT_FILE_SIZE);
    if (generate_temp_file(g_temp_filepath, DEFAULT_FILE_SIZE) != 0) {
      fprintf(stderr, "Failed to generate temp file\n");
      return 1;
    }
    g_temp_file_created = true;
  }

  g_loop = xEventLoopCreate();

  /* ── Phase 1 ── */
  g_phase = 1;
  start_transfer();
  xEventLoopRun(g_loop);
  cleanup_round();
  xEventLoopDestroy(g_loop);
  g_loop = NULL;

  /* ── Phase 2 (resume) ── */
  if (g_resume_test && g_phase1_done) {
    printf("\n── Simulating restart for resume ──\n");
    g_phase                   = 2;
    g_sender_gathering_done   = false;
    g_receiver_gathering_done = false;
    g_sender_done             = false;
    g_receiver_done           = false;
    g_loop                    = xEventLoopCreate();
    start_transfer();
    xEventLoopRun(g_loop);
    cleanup_round();
    xEventLoopDestroy(g_loop);
    g_loop = NULL;
  }

  if (g_temp_file_created) {
    remove(g_temp_filepath);
  }

  return 0;
}
