/*
 * ice_echo.c - ICE loopback echo demo
 *
 * Creates two ICE agents (controlling + controlled) in the same process,
 * exchanges gathered candidates between them, and once connected the
 * controlling side sends "Hello ICE!" which the controlled side echoes
 * back. All three callbacks (on_state_change, on_candidate, on_data)
 * print logs so you can observe the full ICE lifecycle.
 *
 * Usage:
 *   ./ice_echo [-s stun_server:port] [-f candidate_type] [-6]
 *
 * Example:
 *   ./ice_echo -s stun.l.google.com:19302
 *   ./ice_echo -s stun.l.google.com:19302 -f srflx
 *   ./ice_echo -6
 *
 * If no -s flag is given, the default STUN server
 * stun.l.google.com:19302 is used. Pass -s "" to disable STUN.
 *
 * The -f flag filters SDP candidates to only include the given type
 * (e.g. "host", "srflx", "relay"). Useful for testing specific
 * candidate paths.
 *
 * The -6 flag enables IPv6 candidate gathering (disabled by default).
 */

#include <xbase/event.h>
#include <xp2p/ice_agent.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ───────────────────────────────────────────── */

#define MAX_BUFFERED_CANDIDATES 16
#define MAX_CANDIDATE_LEN       256

/* Candidate type filter (NULL = no filter, keep all) */
static const char *g_cand_type_filter = NULL;

/* Whether to enable IPv6 candidate gathering (off by default) */
static bool g_enable_ipv6 = false;

/**
 * Filter SDP string to only keep candidate lines matching the desired type.
 * Non-candidate lines are always kept. Caller must free the returned string.
 * Returns NULL on allocation failure.
 */
static char *filter_sdp_candidates(const char *sdp, const char *type_filter) {
  if (!type_filter || !sdp) return strdup(sdp ? sdp : "");

  size_t sdp_len = strlen(sdp);
  char *out = (char *)malloc(sdp_len + 1);
  if (!out) return NULL;

  size_t out_pos = 0;
  const char *p = sdp;

  while (*p) {
    /* Find end of current line (including \r\n) */
    const char *eol = strstr(p, "\r\n");
    size_t line_len = eol ? (size_t)(eol - p + 2) : strlen(p);

    if (strncmp(p, "a=candidate:", 12) == 0) {
      /* This is a candidate line — check if it matches the filter.
       * We must search only within the current line, not beyond. */
      char match_str[32];
      snprintf(match_str, sizeof(match_str), "typ %s", type_filter);
      const char *found = strstr(p, match_str);
      if (found != NULL && (size_t)(found - p) < line_len) {
        memcpy(out + out_pos, p, line_len);
        out_pos += line_len;
      }
      /* else: skip this candidate line */
    } else {
      /* Non-candidate line — always keep */
      memcpy(out + out_pos, p, line_len);
      out_pos += line_len;
    }

    p += line_len;
  }

  out[out_pos] = '\0';
  return out;
}

static const char *state_name(xIceState s) {
  switch (s) {
  case xIceState_New:       return "New";
  case xIceState_Gathering: return "Gathering";
  case xIceState_Checking:  return "Checking";
  case xIceState_Connected: return "Connected";
  case xIceState_Completed: return "Completed";
  case xIceState_Failed:    return "Failed";
  case xIceState_Closed:    return "Closed";
  default:                  return "Unknown";
  }
}

/* ── Per-agent context ─────────────────────────────────── */

typedef struct AgentCtx AgentCtx;

struct AgentCtx {
  const char *label;       /* "A (controlling)" or "B (controlled)" */
  xIceAgent   self;        /* own agent handle                      */
  AgentCtx   *peer_ctx;    /* peer context (set after create)       */
  xEventLoop  loop;        /* shared event loop                     */
  int         connected;   /* set to 1 when Connected/Completed     */
  int         gathering_done; /* set to 1 when gathering complete   */

  /* Buffered candidates (collected during gathering) */
  char        candidates[MAX_BUFFERED_CANDIDATES][MAX_CANDIDATE_LEN];
  int         candidate_count;
};

/* Forward declaration */
static void try_exchange(AgentCtx *ctx);

/* ── Callbacks ─────────────────────────────────────────── */

static void on_state_change(xIceAgent agent, xIceState state, void *arg) {
  AgentCtx *ctx = (AgentCtx *)arg;
  (void)agent;
  printf("[%s] state -> %s (%d)\n", ctx->label, state_name(state), state);

  if (state == xIceState_Connected || state == xIceState_Completed) {
    ctx->connected = 1;

    /* Controlling side sends the first message once connected */
    if (ctx->self && strstr(ctx->label, "controlling")) {
      const char *msg = "Hello ICE!";
      printf("[%s] sending: %s\n", ctx->label, msg);
      xErrno err = xIceAgentSend(ctx->self, (const uint8_t *)msg, strlen(msg));
      if (err != xErrno_Ok) {
        printf("[%s] send failed: %d\n", ctx->label, err);
      }
    }
  }

  if (state == xIceState_Failed) {
    fprintf(stderr, "[%s] ICE failed, stopping.\n", ctx->label);
    xEventLoopStop(ctx->loop);
  }
}

static void on_candidate(xIceAgent agent, const char *candidate_sdp,
                          void *arg) {
  AgentCtx *ctx = (AgentCtx *)arg;
  (void)agent;

  if (candidate_sdp) {
    printf("[%s] candidate: %s\n", ctx->label, candidate_sdp);

    /* Buffer the candidate */
    if (ctx->candidate_count < MAX_BUFFERED_CANDIDATES) {
      snprintf(ctx->candidates[ctx->candidate_count], MAX_CANDIDATE_LEN,
               "%s", candidate_sdp);
      ctx->candidate_count++;
    }
  } else {
    printf("[%s] gathering complete (end-of-candidates)\n", ctx->label);
    ctx->gathering_done = 1;

    /* If both sides are done gathering, exchange SDP + candidates */
    try_exchange(ctx);
  }
}

static void on_data(xIceAgent agent, const uint8_t *data, size_t len,
                     void *arg) {
  AgentCtx *ctx = (AgentCtx *)arg;
  (void)agent;
  printf("[%s] received (%zu bytes): %.*s\n",
         ctx->label, len, (int)len, (const char *)data);

  /* Echo back if we are the controlled side */
  if (strstr(ctx->label, "controlled")) {
    printf("[%s] echoing back: %.*s\n", ctx->label, (int)len,
           (const char *)data);
    xErrno err = xIceAgentSend(ctx->self, data, len);
    if (err != xErrno_Ok) {
      printf("[%s] echo send failed: %d\n", ctx->label, err);
    }
  } else {
    /* Controlling side received the echo — done! */
    printf("[%s] echo received, success! Stopping.\n", ctx->label);
    xEventLoopStop(ctx->loop);
  }
}

/* ── SDP + Candidate exchange ──────────────────────────── */

/**
 * Called when an agent finishes gathering. If both agents are done,
 * exchange SDP (offer/answer).
 *
 * Since the SDP already contains candidate lines (gathered during
 * xIceAgentGather), SetRemoteDescription will parse them and start
 * connectivity checks automatically.
 */
static void try_exchange(AgentCtx *ctx) {
  AgentCtx *peer = ctx->peer_ctx;
  if (!peer || !ctx->gathering_done || !peer->gathering_done) return;

  /* Only run once (the second agent to finish triggers this) */
  static int exchanged = 0;
  if (exchanged) return;
  exchanged = 1;

  /* Determine which is A (controlling) and which is B (controlled) */
  AgentCtx *a_ctx = strstr(ctx->label, "controlling") ? ctx : peer;
  AgentCtx *b_ctx = (a_ctx == ctx) ? peer : ctx;

  char *offer = xIceAgentCreateOffer(a_ctx->self);
  if (offer) {
    /* Apply candidate type filter if set */
    char *filtered_offer = filter_sdp_candidates(offer, g_cand_type_filter);
    printf("\n[demo] offer:\n%s\n", filtered_offer ? filtered_offer : offer);
    xIceAgentSetRemoteDescription(b_ctx->self,
                                   filtered_offer ? filtered_offer : offer);
    free(offer);
    free(filtered_offer);
  }

  char *answer = xIceAgentCreateAnswer(b_ctx->self);
  if (answer) {
    /* Apply candidate type filter if set */
    char *filtered_answer = filter_sdp_candidates(answer, g_cand_type_filter);
    printf("[demo] answer:\n%s\n", filtered_answer ? filtered_answer : answer);
    xIceAgentSetRemoteDescription(a_ctx->self,
                                   filtered_answer ? filtered_answer : answer);
    free(answer);
    free(filtered_answer);
  }

  printf("[demo] exchange complete, connectivity checks starting...\n\n");
}

/* ── Timeout ───────────────────────────────────────────── */

static void timeout_cb(void *arg) {
  xEventLoop loop = (xEventLoop)arg;
  fprintf(stderr, "[demo] timeout — no connectivity after 10s, stopping.\n");
  xEventLoopStop(loop);
}

/* ── Main ──────────────────────────────────────────────── */

static const char *DEFAULT_STUN_SERVER = "stun.l.google.com:19302";

int main(int argc, char *argv[]) {
  const char *stun_server = DEFAULT_STUN_SERVER;

  int opt;
  while ((opt = getopt(argc, argv, "s:f:6")) != -1) {
    switch (opt) {
    case 's':
      stun_server = optarg[0] ? optarg : NULL;
      break;
    case 'f':
      g_cand_type_filter = optarg;
      break;
    case '6':
      g_enable_ipv6 = true;
      break;
    default:
      fprintf(stderr,
              "Usage: %s [-s stun_server:port] [-f candidate_type] [-6]\n",
              argv[0]);
      return 1;
    }
  }

  printf("=== ICE Loopback Echo Demo ===\n");
  printf("[demo] STUN server: %s\n", stun_server ? stun_server : "(none)");
  printf("[demo] candidate filter: %s\n",
         g_cand_type_filter ? g_cand_type_filter : "(all)");
  printf("[demo] IPv6: %s\n\n", g_enable_ipv6 ? "enabled" : "disabled");

  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    fprintf(stderr, "Failed to create event loop\n");
    return 1;
  }

  /* ── Agent contexts ── */
  AgentCtx ctx_a;
  memset(&ctx_a, 0, sizeof(ctx_a));
  ctx_a.label = "A (controlling)";
  ctx_a.loop  = loop;

  AgentCtx ctx_b;
  memset(&ctx_b, 0, sizeof(ctx_b));
  ctx_b.label = "B (controlled)";
  ctx_b.loop  = loop;

  /* ── Create agents (no SDP exchange yet — just create) ── */
  xIceConf conf_a;
  memset(&conf_a, 0, sizeof(conf_a));
  conf_a.role            = xIceRole_Controlling;
  conf_a.stun_server     = stun_server;
  conf_a.on_state_change = on_state_change;
  conf_a.on_candidate    = on_candidate;
  conf_a.on_data         = on_data;
  conf_a.enable_ipv6     = g_enable_ipv6;
  conf_a.callback_arg    = &ctx_a;

  xIceConf conf_b;
  memset(&conf_b, 0, sizeof(conf_b));
  conf_b.role            = xIceRole_Controlled;
  conf_b.stun_server     = stun_server;
  conf_b.on_state_change = on_state_change;
  conf_b.on_candidate    = on_candidate;
  conf_b.on_data         = on_data;
  conf_b.enable_ipv6     = g_enable_ipv6;
  conf_b.callback_arg    = &ctx_b;

  xIceAgent agent_a = xIceAgentCreate(loop, &conf_a);
  xIceAgent agent_b = xIceAgentCreate(loop, &conf_b);
  if (!agent_a || !agent_b) {
    fprintf(stderr, "Failed to create ICE agents\n");
    xIceAgentDestroy(agent_a);
    xIceAgentDestroy(agent_b);
    xEventLoopDestroy(loop);
    return 1;
  }

  ctx_a.self     = agent_a;
  ctx_a.peer_ctx = &ctx_b;
  ctx_b.self     = agent_b;
  ctx_b.peer_ctx = &ctx_a;

  printf("[demo] agents created\n");

  /* ── Start gathering on both agents ── */
  printf("[demo] starting candidate gathering...\n");
  xIceAgentGather(agent_a);
  xIceAgentGather(agent_b);

  /* SDP + candidate exchange happens in on_candidate callback
   * once both agents finish gathering. */

  /* ── Safety timeout ── */
  xEventLoopTimerAfter(loop, timeout_cb, loop, 10000);

  /* ── Run ── */
  printf("[demo] running event loop...\n\n");
  xEventLoopRun(loop);

  /* ── Cleanup ── */
  printf("\n[demo] cleaning up...\n");
  xIceAgentDestroy(agent_a);
  xIceAgentDestroy(agent_b);
  xEventLoopDestroy(loop);

  printf("[demo] done.\n");
  return 0;
}
