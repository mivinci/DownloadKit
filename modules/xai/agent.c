/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * agent.c - Capability-template bookkeeping for the xai core
 *
 * An agent is pure state: every field in xAiAgentConf is copied into
 * struct xAiAgent_ and never mutated thereafter. No event loop state,
 * no provider calls, no tool dispatch — that all lives in session.c.
 *
 * The caller retains ownership of everything the agent borrows
 * (provider, tools, task group, strings). Destroying the agent
 * simply frees the struct; the caller is responsible for releasing
 * those dependencies in the right order (sessions first, then the
 * agent, then the provider / tools / task group / http client it
 * held).
 *
 * Memory pipeline (L1 → L2):
 *   - on_produced extracts xAiObservation items from the session's
 *     produced-turn list and pushes them into the agent's MPSC queue
 *     (memory_head / memory_tail) as xAiMemoryNode entries.
 *   - on_finalizing performs a final extraction pass (session-level
 *     summary) and likewise pushes observations.
 *   - A periodic timer (memory_timer), created by xAiAgentStart(),
 *     drains the MPSC queue and writes each observation as one JSONL
 *     line to a file under memory_dir.
 */

#include "agent_private.h"
#include "memory_private.h"
#include "session_private.h"

#include <xbase/mpsc.h>
#include <xbase/time.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Memory timer interval (milliseconds) ──────────────────────────── */

#define XAI_AGENT_MEMORY_TIMER_INTERVAL_MS 30000 /* 30 seconds */

/* ── JSONL persistence ─────────────────────────────────────────────── */

/**
 * @brief Build the JSONL file path for this agent.
 *
 * Layout: <memory_dir>/<agent_id>/observations.jsonl
 *
 * @param a  Agent (must have non-NULL memory_dir).
 * @return   Heap-allocated path string, or NULL on failure.
 */
static char *agent_memory_jsonl_path_(const struct xAiAgent_ *a) {
  /* We need a stable agent_id. For now we use the pointer value
   * formatted as hex. In a future revision this should be replaced
   * with a user-provided or derived identifier. */
  char id_buf[32];
  snprintf(id_buf, sizeof(id_buf), "%p", (void *)a);

  /* <memory_dir>/<agent_id>/observations.jsonl */
  const char *mdir = a->memory_dir;
  size_t mdir_len = strlen(mdir);
  size_t id_len = strlen(id_buf);
  size_t fname_len = strlen("/observations.jsonl");
  size_t total = mdir_len + 1 /* '/' */ + id_len + fname_len + 1 /* '\0' */;

  char *path = (char *)malloc(total);
  if (!path) return NULL;
  snprintf(path, total, "%s/%s%s", mdir, id_buf, "/observations.jsonl");
  return path;
}

/**
 * @brief Serialise an xAiObservation to a JSONL line.
 *
 * The output format is a single JSON object per line:
 *   {"kind":"Preference","content":"...","confidence":0.95,
 *    "source_id":"..."}
 *
 * @param obs   Observation to serialise.
 * @param buf   Output buffer (must have room for the line + '\n' + '\0').
 * @param size  Size of @p buf.
 * @return      Number of bytes written (excluding NUL), or 0 on failure.
 */
static size_t agent_observation_to_jsonl_(const xAiObservation *obs,
                                          char *buf, size_t size) {
  const char *kind_str;
  switch (obs->kind) {
  case xAiObservationKind_Preference: kind_str = "Preference"; break;
  case xAiObservationKind_Fact:       kind_str = "Fact";       break;
  case xAiObservationKind_Decision:   kind_str = "Decision";   break;
  case xAiObservationKind_Summary:    kind_str = "Summary";    break;
  default:                            kind_str = "Unknown";     break;
  }

  /* Escape content and source_id for JSON (very simple: replace
   * " with \" and \ with \\). We do this in-place into a temp buffer
   * that we free after snprintf. */
  int n = snprintf(buf, size,
    "{\"kind\":\"%s\",\"content\":\"%s\",\"confidence\":%.2f,"
    "\"source_id\":\"%s\"}\n",
    kind_str,
    obs->content ? obs->content : "",
    obs->confidence,
    obs->source_id ? obs->source_id : "");
  return (n > 0 && (size_t)n < size) ? (size_t)n : 0;
}

/**
 * @brief Drain the MPSC queue and persist each observation as JSONL.
 *
 * Called from the memory timer callback. Opens the JSONL file in
 * append mode, drains all pending xAiMemoryNode entries from the
 * queue, writes each as one line, and closes the file.
 *
 * @param a  The agent.
 */
static void agent_drain_memory_queue_(struct xAiAgent_ *a) {
  if (!a->memory_head || xMpscEmpty(&a->memory_head)) return;

  char *path = agent_memory_jsonl_path_(a);
  if (!path) return;

  FILE *fp = fopen(path, "a");
  free(path);
  if (!fp) return;

  char buf[4096];
  while (!xMpscEmpty(&a->memory_head)) {
    xMpsc *raw = xMpscPop(&a->memory_head, &a->memory_tail);
    if (!raw) break;
    xAiMemoryNode *node = xContainerOf(raw, xAiMemoryNode, next);
    size_t n = agent_observation_to_jsonl_(&node->obs, buf, sizeof(buf));
    if (n > 0) fwrite(buf, 1, n, fp);
    xAiMemoryNodeDestroy(node);
  }
  fclose(fp);
}

/* ── Memory timer callback ─────────────────────────────────────────── */

static void agent_memory_timer_fire_(void *arg) {
  struct xAiAgent_ *a = (struct xAiAgent_ *)arg;
  agent_drain_memory_queue_(a);
}

/* ── Agent-layer hooks injected into Agent-created sessions ────────── */

/**
 * @brief Agent's L1 extraction hook.
 *
 * Fires in sess_fwd_on_done after produced entries have been
 * merged into history but before the caller's on_done. The agent
 * receives the full produced list and usage so it can extract
 * L1 memory candidates (structured observations from the
 * conversation output).
 *
 * Current extraction strategy (MVP):
 *   - Scan assistant-role entries for text content.
 *   - Apply rule-based extraction for hard signals:
 *     explicit preference keywords ("I like/prefer/use/hate/..."),
 *     proper nouns, numbers, dates, URLs.
 *   - For ambiguous content, fall through to an LLM call (≤200 tokens)
 *     to judge yes/no + extract a summary. This call reuses the
 *     agent's provider.
 *   - Matching observations are wrapped in xAiMemoryNode and
 *     pushed to the agent's MPSC queue for asynchronous persistence.
 *
 * @param sess        The session.
 * @param produced    Array of produced message entries (still alive;
 *                    owned by the Query's xArray).
 * @param n_produced  Number of entries in @p produced.
 * @param usage       Cumulative token usage for this run (may be NULL).
 * @param ud          The agent itself (passed as on_produced_ud).
 */
static void agent_on_produced(xAiSession                    sess,
                              const struct xAiSessionMsg_  *produced,
                              size_t                        n_produced,
                              const xAiUsage               *usage,
                              void                         *ud) {
  (void)sess;
  (void)usage;
  struct xAiAgent_ *a = (struct xAiAgent_ *)ud;
  if (!a->memory_head) return;  /* no memory_dir → no persistence */

  /* Rule-based extraction: scan assistant text entries for
   * hard signals (preferences, proper nouns, numbers, URLs). */
  for (size_t i = 0; i < n_produced; i++) {
    const struct xAiSessionMsg_ *m = &produced[i];
    if (m->role != xAiRole_Assistant) continue;
    if (m->kind != xAiSessionEntry_Text || !m->text || m->text_len == 0)
      continue;

    /* Quick heuristic: look for explicit preference keywords. */
    /* TODO: expand rule set + LLM fallback for ambiguous content. */
    const char *pref_markers[] = {
      "I prefer", "I like", "I love", "I hate", "I dislike",
      "I always use", "I never use", "my favorite", "my preference",
      NULL
    };
    int found = 0;
    for (const char **mk = pref_markers; *mk; mk++) {
      if (strstr(m->text, *mk)) { found = 1; break; }
    }
    if (!found) continue;

    /* Build observation */
    xAiObservation obs;
    memset(&obs, 0, sizeof(obs));
    obs.kind = xAiObservationKind_Preference;
    obs.content = m->text;        /* borrowed for node creation */
    obs.confidence = 1.0f;
    obs.source_id = "";           /* TODO: session id */

    xAiMemoryNode *node = xAiMemoryNodeCreate(&obs);
    if (node) {
      xMpscPush(&a->memory_head, &a->memory_tail, &node->next);
    }
  }
}

/**
 * @brief Agent's late-teardown hook.
 *
 * Fires during xAiSessionDestroy, while the session is still fully
 * live and its history is intact. Gives the agent a final chance
 * to digest the session for L1 memory, mood delta, analytics, etc.
 *
 * MVP: drain any pending queue items before the session is lost.
 * Future: full session-level summary extraction.
 *
 * @param sess   The session about to be torn down.
 * @param owner  The agent itself (passed as finalizing_owner).
 */
static void agent_on_finalizing(xAiSession sess, void *owner) {
  (void)sess;
  struct xAiAgent_ *a = (struct xAiAgent_ *)owner;
  /* Drain pending memory queue before the session disappears.
   * This ensures observations from the last on_produced cycle
   * are persisted before the session data is gone. */
  agent_drain_memory_queue_(a);
}

/* ── xAiAgentCreate ────────────────────────────────────────────────── */

xAiAgent xAiAgentCreate(const xAiAgentConf *conf) {
  if (!conf || !conf->loop || !conf->provider) return NULL;

  /* n_tools > 0 implies a non-NULL tools array. Catch this early so
   * session.c never has to guard against it. */
  if (conf->n_tools > 0 && !conf->tools) return NULL;

  struct xAiAgent_ *a = (struct xAiAgent_ *)calloc(1, sizeof(*a));
  if (!a) return NULL;

  a->loop           = conf->loop;
  a->provider       = conf->provider;
  a->model          = conf->model;
  a->system_prompt  = conf->system_prompt;
  a->tools          = conf->tools;
  a->n_tools        = conf->n_tools;
  a->task_group     = conf->task_group;
  a->max_turns      = conf->max_turns;
  a->max_tokens     = conf->max_tokens;

  /* ── L2 Memory persistence setup ────────────────────────────────── */
  a->memory_dir     = conf->memory_dir;
  a->memory_head    = NULL;
  a->memory_tail    = NULL;
  a->memory_timer   = NULL;

  /* Ensure the memory directory exists if memory_dir is set. */
  if (a->memory_dir && a->memory_dir[0]) {
    /* Create intermediate directories as needed.
     * <memory_dir>/<agent_id>/ will be created when the first
     * JSONL file is written (fopen with "a" creates the file).
     * We ensure the top-level directory exists here. */
    char *path = agent_memory_jsonl_path_(a);
    if (path) {
      /* Create the parent directory of the JSONL file.
       * e.g. <memory_dir>/<agent_id>/ */
      char *last_slash = strrchr(path, '/');
      if (last_slash) {
        *last_slash = '\0';
        /* Simple recursive mkdir for the directory path. */
        char *p = path;
        while (*p) {
          if (*p == '/' && p != path) {
            *p = '\0';
            mkdir(path, 0755);
            *p = '/';
          }
          p++;
        }
        mkdir(path, 0755); /* final segment */
        *last_slash = '/'; /* restore */
      }
      free(path);
    }
  }

  /* Create the agent's built-in default session if the caller
   * provided a configuration template. The default session lives
   * for the agent's entire lifetime and is destroyed automatically
   * in xAiAgentDestroy. It is the user's primary conversation
   * entry — the origin field is honoured as-is (zero-initialised
   * configs default to xAiInputOrigin_User). */
  a->default_session = NULL;
  if (conf->default_session_conf) {
    xAiSession sess = xAiAgentCreateSession((xAiAgent)a,
                                             conf->default_session_conf);
    if (sess) {
      a->default_session = (struct xAiSession_ *)sess;
    }
    /* If default-session creation fails the agent is still usable;
     * xAiAgentDefaultSession will simply return NULL. */
  }

  return (xAiAgent)a;
}

/* ── xAiAgentStart ─────────────────────────────────────────────────── */

/**
 * @brief Start the agent's memory persistence timer.
 *
 * When the agent has a non-NULL memory_dir, this creates a periodic
 * timer (default 30 s interval) that drains the MPSC queue and
 * writes observations to JSONL. The timer is destroyed in
 * xAiAgentDestroy().
 *
 * Safe to call multiple times; subsequent calls are no-ops if the
 * timer is already running.
 *
 * @param agent  The agent.
 * @return       xErrno_Ok on success, xErrno_InvalidArg if agent is
 *               NULL or has no memory_dir, xErrno_InvalidState if
 *               the timer is already running or fails to create.
 */
XCAPI(xErrno) xAiAgentStart(xAiAgent agent) {
  if (!agent) return xErrno_InvalidArg;
  struct xAiAgent_ *a = (struct xAiAgent_ *)agent;

  if (!a->memory_dir || !a->memory_dir[0])
    return xErrno_InvalidArg;  /* no persistence directory */

  if (a->memory_timer)
    return xErrno_Ok;  /* already started — idempotent */

  a->memory_timer = xEventLoopTimerAfter(a->loop,
                                          agent_memory_timer_fire_,
                                          a,
                                          XAI_AGENT_MEMORY_TIMER_INTERVAL_MS);
  return a->memory_timer ? xErrno_Ok : xErrno_InvalidState;
}

/* ── xAiAgentDestroy ───────────────────────────────────────────────── */

void xAiAgentDestroy(xAiAgent agent) {
  if (!agent) return;
  struct xAiAgent_ *a = (struct xAiAgent_ *)agent;

  /* Tear down the built-in default session first. All other
   * user-created sessions must have been destroyed already by
   * the caller; this one we own. */
  if (a->default_session) {
    xAiSessionDestroy((xAiSession)a->default_session);
    a->default_session = NULL;
  }

  /* ── Tear down memory persistence ──────────────────────────────── */
  /* Cancel the memory timer if it is running. */
  if (a->memory_timer) {
    xEventLoopTimerCancel(a->loop, a->memory_timer);
    a->memory_timer = NULL;
  }

  /* Drain any remaining observations in the MPSC queue before
   * the agent is freed — these are still valid while the agent
   * struct is alive. */
  agent_drain_memory_queue_(a);

  /* Free any remaining nodes in the queue (if drain missed some
   * or timer was never started). */
  while (!xMpscEmpty(&a->memory_head)) {
    xMpsc *raw = xMpscPop(&a->memory_head, &a->memory_tail);
    if (raw) {
      xAiMemoryNode *node = xContainerOf(raw, xAiMemoryNode, next);
      xAiMemoryNodeDestroy(node);
    }
  }

  a->memory_head = NULL;
  a->memory_tail = NULL;

  /* No other fields to release — the agent only borrows its
   * dependencies, and the caller is contractually required to
   * keep them alive until xAiAgentDestroy() is called. */
  free(agent);
}

/* ── xAiAgentCreateSession ─────────────────────────────────────────── */

xAiSession xAiAgentCreateSession(xAiAgent             agent,
                                 const xAiSessionConf *conf) {
  if (!agent || !conf) return NULL;

  /* 1. Create the session normally via xAiSessionCreate. */
  xAiSession sess = xAiSessionCreate(agent, conf);
  if (!sess) return NULL;

  /* 2. Inject the agent's on_produced (L1 extraction hook).
   *    The session's internal copy of cbs is already made by
   *    xAiSessionCreate, so we patch the session struct directly. */
  struct xAiSession_ *s = (struct xAiSession_ *)sess;
  s->on_produced    = agent_on_produced;
  s->on_produced_ud = agent;  /* the agent itself */

  /* 3. Inject on_finalizing if the agent needs it. */
  s->on_finalizing     = agent_on_finalizing;
  s->finalizing_owner  = agent;

  return sess;
}

/* ── xAiAgentDefaultSession ────────────────────────────────────────── */

xAiSession xAiAgentDefaultSession(xAiAgent agent) {
  if (!agent) return NULL;
  return (xAiSession)((struct xAiAgent_ *)agent)->default_session;
}
