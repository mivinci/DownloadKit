/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * main.cpp - moo command-line entry.
 *
 * Streaming REPL driven by the full xagent stack
 * (xAgent + xAgentSession + xAgentTool + xAgentProvider).
 *
 * Unlike ai_openai (which drives the provider vtable directly for
 * end-to-end diagnostics), this is the canonical integration path
 * that user code should copy:
 *
 *   xAgentProvider -> xAgent -> xAgentSession
 *
 * The session hides the tool-call loop entirely: when the model asks
 * to call a tool, the session invokes the registered handler, folds
 * the result back into history, and submits another round on its
 * own. The REPL only sees streamed text plus a single on_done event
 * per user input.
 *
 * Usage:
 *   ./moo [-d, --data-dir <path>]                   # default: cwd
 *
 * Model configuration lives in <data_dir>/models.json. The file is
 * required — startup fails fast with a helpful error if it's
 * missing or malformed. See apps/cli/config.h for the schema and
 * a minimal example.
 *
 * The REPL uses xline for CJK-aware line editing, persistent history
 * (stored at <data_dir>/.ai_session_history), and Ctrl-R reverse
 * search. See cmake/FindIsocline.cmake.
 *
 * Translation units:
 *   ctx.h        - shared ReplCtx + PendingConfirm types
 *   output.*     - above_printf / above_chunk / end_thinking
 *   slash.*      - slash command table, handlers, Tab completer
 *   callbacks.*  - on_text / on_thinking / on_tool / on_done / ...
 *   repl.*       - editor lifecycle, confirm gate, line dispatch,
 *                  submit helper, SIGINT watcher
 *   banner.*     - startup banner: logo table + bordered-box print
 *   main.cpp     - this file: argv parsing, object wiring, banner
 *                  invocation, event loop drive, shutdown
 */

#include "banner.h"
#include "callbacks.h"
#include "config.h"
#include "ctx.h"
#include "output.h"
#include "repl.h"
#include "slash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <unistd.h>

#include <xagent/agent.h>
#include <xagent/memory.h>
#include <xagent/model.h>
#include <xagent/provider.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xagent/tool_shell.h>
#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xbase/flag.h>
#include <xhttp/client.h>
#include <xline/line.h>

int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  /* ── All C++ objects with constructors are declared up front so
   *    the goto-based cleanup below never jumps over an
   *    initialisation (which would be ill-formed). Default-
   *    constructed values are harmless; they get populated as the
   *    corresponding resources come online. ─────────────────── */
  int               rc = 0;
  CliModelConfig    model_cfg;
  ReplCtx           ctx;
  xAgentShellConf   shell_conf;
  xAgentSessionConf sconf;
  xAgentConf        aconf;
  std::memset(&shell_conf, 0, sizeof(shell_conf));
  std::memset(&sconf, 0, sizeof(sconf));
  std::memset(&aconf, 0, sizeof(aconf));

  /* Owned resources; each assigned to non-null on successful
   * acquire. Cleanup labels at the bottom release them in reverse
   * order. Keep this table and the label ladder in sync. */
  xFlagSet      fset        = nullptr;
  xEventLoop    loop        = nullptr;
  xHttpClient   http        = nullptr;
  xAgentTool    shell_tool  = nullptr;
  xAgentMemory  memory_store = nullptr;
  xAgent        agent       = nullptr;
  bool          repl_open   = false;
  bool          sig_armed   = false;

  /* ── Parse command-line options ───────────────────────────────────
   *
   * Single knob for now: -d/--data-dir. Going through xbase/flag
   * instead of getopt(3) gets us GNU long options, a typed storage
   * pointer, an auto-generated --help with [default: ...] annotation,
   * and forward-compat with the future xcli subcommand module — all
   * without dragging in another dependency. `data_dir_arg` points
   * into argv on success (zero-copy, same convention as optarg); we
   * copy into `cwd_buf` only on the fallback path. */
  const char *data_dir_arg = nullptr;
  fset = xFlagSetCreate("moo", "moo command-line tool");
  if (!fset) {
    std::fprintf(stderr, "failed to create flag set\n");
    rc = 1;
    goto out;
  }
  /* Wire --version / -V to the CMake-injected build version so
   * `moo --version` stays in lockstep with the banner. xFlagParse
   * prints and returns xErrno_Again (handled below). */
  xFlagSetVersion(fset, MOO_VERSION);
  xFlagAddString(fset, "data-dir", 'd', "PATH",
                 "data directory (history, agent state)", &data_dir_arg,
                 nullptr, xFlagAttr_None);

  {
    char  *flag_err = nullptr;
    xErrno frc      = xFlagParse(fset, argc, argv, &flag_err);
    if (frc == xErrno_Again) {
      /* --help / --version handled; text already on stdout. */
      rc = 0;
      goto out_flagset;
    }
    if (frc != xErrno_Ok) {
      std::fprintf(stderr, "%s\n", flag_err ? flag_err : "parse error");
      std::free(flag_err);
      xFlagPrintUsage(fset, stderr);
      rc = 1;
      goto out_flagset;
    }
  }

  {
    /* Default data_dir to the current working directory. */
    char        cwd_buf[4096];
    const char *data_dir = data_dir_arg;
    if (!data_dir) {
      if (getcwd(cwd_buf, sizeof(cwd_buf))) {
        data_dir = cwd_buf;
      } else {
        data_dir = ".";
      }
    }

    /* data_dir_arg (when set) points into argv, which lives for the
     * whole of main; the flag set can be torn down now that we've
     * captured everything we need into locals. */
    xFlagSetDestroy(fset);
    fset = nullptr;

    /* ── Event loop + HTTP client ────────────────────────────── */
    loop = xEventLoopCreate();
    if (!loop) {
      std::fprintf(stderr, "failed to create event loop\n");
      rc = 1;
      goto out;
    }
    http = xHttpClientCreate(loop, nullptr);
    if (!http) {
      std::fprintf(stderr, "failed to create http client\n");
      rc = 1;
      goto out_loop;
    }

    /* ── Load <data_dir>/models.json ────────────────────────────
     *
     * Non-fatal if the file is simply missing: cli_model_config_load
     * hands back an empty CliModelConfig (registry created but no
     * entries, default_id empty) and we drop into degraded mode —
     * agent/session are skipped, the REPL still starts so the user
     * can reach /help, /exit, etc. Any *other* error (malformed
     * JSON, unknown "kind", "default" not in "models", I/O error
     * other than ENOENT) is still fatal, because in those cases the
     * user wrote a file and got it wrong — silently degrading would
     * hide the mistake. The config owns every created provider plus
     * the registry; teardown goes through cli_model_config_destroy
     * at the end of main. */
    {
      std::string cfg_err;
      if (cli_model_config_load(data_dir, loop, http, &model_cfg, &cfg_err) !=
          0) {
        std::fprintf(stderr, "config: %s\n", cfg_err.c_str());
        rc = 1;
        goto out_http;
      }
    }

    /* Degraded "no model configured" mode: models.json was absent,
     * so the registry is empty and default_id is "". Chat submits
     * would have nothing to route to, so we skip xAgentCreate
     * entirely. ctx.sess stays nullptr, which repl_handle_line
     * uses as the sentinel to reject chat input with a helpful
     * hint while letting every slash command (including /model,
     * which will correctly print "(no models registered)") keep
     * working. */
    const bool no_models = model_cfg.default_id.empty();

    /* ── Tools ─────────────────────────────────────────────────── */
    ctx.loop           = loop;
    ctx.model_registry = model_cfg.registry;
    ctx.model_cfg      = &model_cfg;

    shell_conf.callback_ud = &ctx;
    shell_conf.on_command  = [](const char *command, const char *cwd,
                               void       *ud) {
      auto *c = static_cast<ReplCtx *>(ud);
      if (cwd && cwd[0]) {
        above_printf(c->line, "\x1b[2m  $ (cd %s && %s)\x1b[0m", cwd, command);
      } else {
        above_printf(c->line, "\x1b[2m  $ %s\x1b[0m", command);
      }
    };
    shell_conf.on_result = [](int exit_code, size_t stdout_len,
                              size_t stderr_len, int timed_out, void *ud) {
      auto *c = static_cast<ReplCtx *>(ud);
      above_printf(c->line, "\x1b[2m  exit=%d stdout=%zu stderr=%zu%s\x1b[0m",
                   exit_code, stdout_len, stderr_len,
                   timed_out ? " (timed out)" : "");
    };
    shell_tool = xAgentToolShellCreate(loop, &shell_conf);
    if (!shell_tool) {
      std::fprintf(stderr, "failed to create shell tool\n");
      rc = 1;
      goto out_cfg;
    }

    const xAgentTool *tool_ptrs[] = {&shell_tool};
    const size_t      TOTAL_TOOLS = 1;

    /* ── Session config (agent's default session) ────────────────
     *
     * Instead of creating a session manually and managing its
     * lifecycle, we set default_session_conf on the agent so it
     * creates a built-in default session at construction time.
     * The session is retrieved via xAgentDefaultSession() and
     * is destroyed automatically by xAgentDestroy(). */
    sconf.cbs.on_text         = on_text;
    sconf.cbs.on_thinking     = on_thinking;
    sconf.cbs.on_tool         = on_tool;
    sconf.cbs.on_tool_output  = on_tool_output;
    sconf.cbs.on_sidecar      = on_sidecar;
    sconf.cbs.on_done         = on_done;
    sconf.cbs.on_error        = on_error;
    sconf.cbs.on_tool_confirm = on_tool_confirm;
    sconf.cbs.user_data       = &ctx;

    /* Opt into the structured budget pipeline so the calibrator
     * actually runs. Without a non-Disabled policy the gate short-
     * circuits, last_prompt_estimate stays zero, and on_done's
     * calibrator update bails out — factor would forever read 1.0
     * and samples 0, defeating the whole point of this demo.
     *
     * The 8192 fallback was picked empirically: large enough that a
     * single long-form answer (think: a derivation with multi-paragraph
     * reasoning) plus the floor pinned by keep_recent_turns won't
     * trip the gate on turn #2, but small enough that a handful of
     * sustained turns will eventually push the rolling history past
     * the cap and exercise TruncateOldest. Per-model
     * "context_window" in models.json overrides it for models whose
     * real window is meaningfully larger (e.g. 128k for kimi-k2) or
     * smaller (e.g. 8k for a local llama.cpp server); /model
     * switches re-apply the selected entry's value so the budget
     * gate always matches the active backend. keep_recent_turns
     * =2 is the floor — the current user turn and the immediately
     * prior assistant turn are never discarded, so the model keeps
     * local context even when the trimmer fires. If you shrink
     * max_tokens below ~4096 expect xErrno_PromptTooLong (which the
     * REPL and on_error both surface with a hint line below), and
     * see session.c's keep_recent_turns floor logic for why. */
    constexpr size_t kDefaultContextWindow = 8192;
    size_t           session_max_tokens    = kDefaultContextWindow;
    if (!no_models) {
      const CliModelEntry *def =
        cli_model_config_find(&model_cfg, model_cfg.default_id.c_str());
      if (def && def->context_window > 0) {
        session_max_tokens = def->context_window;
      }
    }
    ctx.default_context_window = kDefaultContextWindow;

    sconf.budget.policy            = xAgentBudgetPolicy_Auto;
    sconf.budget.max_tokens        = session_max_tokens;
    sconf.budget.keep_recent_turns = 2;
    sconf.budget.on_budget_event   = on_budget_event;
    sconf.budget.budget_event_ud   = &ctx;

    /* Sidecar idle timeout: when an async tool (e.g. shell) has not
     * produced output for 3 seconds, launch a sidecar Query so the
     * AI can inspect the situation and decide what to do next. Zero
     * would disable the sidecar mechanism entirely. */
    sconf.sidecar_idle_ms = 3000;

    /* ── Agent ──────────────────────────────────────── */
    aconf.loop = loop;
    /* Registry path: the agent resolves default_model_id against the
     * registry at create time and keeps a borrowed reference so the
     * REPL's /model command can flip sessions between registered
     * specs without tearing the agent down. */
    aconf.model_registry   = model_cfg.registry;
    aconf.default_model_id = model_cfg.default_id.c_str();
    aconf.system_prompt =
      "You are MOO, a concise AI assistant that lives in the user's "
      "terminal. You have access to a shell tool that runs commands "
      "via /bin/sh -c and returns stdout, stderr, and the exit code; "
      "use it whenever you need to run commands, inspect the system, "
      "or compute something. You may chain multiple tool calls in a "
      "single turn. Keep replies short.";
    aconf.tools                = tool_ptrs;
    aconf.tools_count          = TOTAL_TOOLS;
    /* Tool-loop cap: models.json's top-level "max_turns" wins; if
     * the key is absent or non-positive (encoded as 0 by the loader)
     * we fall back to the built-in 64, which is generous enough for
     * most agentic tasks without letting a runaway loop burn through
     * quota on its own. */
    aconf.max_turns            = model_cfg.max_turns > 0 ? model_cfg.max_turns : 64;
    /* ── Pluggable long-term memory ──────────────────────────────
     *
     * The built-in JSONL backend lays out one file per session
     * under <data_dir>/sessions/<session_id>/memory.jsonl. The
     * agent wires it into every session it mints (append on
     * preserve, prime on create) so conversations resume across
     * process runs when the caller reuses a stable session_id.
     *
     * Stays alive past xAgentDestroy so the teardown ladder can
     * release it *after* the agent has torn its sessions down. */
    xAgentMemoryJsonlConf mconf;
    std::memset(&mconf, 0, sizeof(mconf));
    mconf.root_dir = data_dir;
    memory_store   = xAgentMemoryJsonlCreate(&mconf);
    if (!memory_store) {
      std::fprintf(stderr, "failed to create memory store\n");
      rc = 1;
      goto out_tool;
    }
    aconf.memory               = memory_store;
    aconf.enable_sidecar_query = 1;
    aconf.default_session_conf = &sconf;

    /* Only build the agent when we actually have a model to route
     * to. In degraded mode (no_models) ctx.sess stays nullptr and
     * the chat path is gated off in repl_handle_line; the tools,
     * registry, loop and http client are still wired up so the
     * rest of the REPL machinery behaves normally. */
    if (!no_models) {
      agent = xAgentCreate(&aconf);
      if (!agent) {
        std::fprintf(stderr, "failed to create agent\n");
        rc = 1;
        goto out_tool;
      }

      /* Retrieve the agent's built-in default session — no manual
       * create/destroy needed. The session lives for the agent's
       * entire lifetime. */
      xAgentSession sess = xAgentDefaultSession(agent);
      if (!sess) {
        std::fprintf(stderr, "agent has no default session\n");
        rc = 1;
        goto out_agent;
      }

      ctx.sess             = sess;
      ctx.current_model_id = model_cfg.default_id;
    }

    /* ── Startup banner ─────────────────────────────────────────
     *
     * Printed once in cooked mode before repl_open_line paints the
     * prompt. The layout, logo table and width accounting all live
     * in banner.cpp; we just feed it the knobs to show on the right
     * column. `model_label` is synthesised here as "<name> (id=<id>)"
     * so the printed line keeps the same shape as before: it tells
     * you both what's actually hitting the wire and what slash-
     * command id selects it. The label length is bounded by the
     * registry; banner_print truncates to fit the right column. */
    {
      char label[192];
      label[0] = 0;
      if (!no_models) {
        const xAgentModelSpec *dspec = xAgentModelRegistryGet(
          model_cfg.registry, model_cfg.default_id.c_str());
        const char *dmodel = dspec && dspec->model ? dspec->model : "?";
        std::snprintf(label, sizeof(label), "%s (id=%s)", dmodel,
                      model_cfg.default_id.c_str());
      }
      banner_print(MOO_VERSION, label, "shell", data_dir, no_models ? 1 : 0);
    }

    /* ── Line editor (xline) ──────────────────────────────────────
     *
     * Persist history under the agent's data_dir so each agent
     * namespace keeps its own recall buffer. 1000 entries is plenty
     * for a chat REPL — isocline prunes oldest on overflow. Passing
     * NULL would disable persistence (history kept only in-memory);
     * passing a path enables both load-on-start and save-on-exit.
     *
     * We don't keep the path anywhere: /history now enumerates the
     * in-memory (decoded) buffer via xLineHistoryGet, so there's no
     * need to re-read the on-disk file (which is stored in
     * isocline's internal \xHH-escaped format and not directly
     * printable). */
    char hist_path[4096];
    std::snprintf(hist_path, sizeof(hist_path), "%s/.ai_session_history",
                  data_dir);
    xLineSetHistory(hist_path, 1000);

    /* Slash-command completion. isocline calls slash_completer on Tab;
     * when the current prefix resolves to a single match it ALSO shows
     * the tail as an inline faint hint (fish-shell style) without
     * needing Tab. Hints are on by default, but we set them explicitly
     * so a future xLineEnableHint(false) somewhere else doesn't silently
     * break the UX. completion_preview (the faint candidate shown while
     * the menu is open) is already on by default; left as-is. */
    slash_install_completer(&ctx);
    xLineEnableHint(true);
    /* Auto-open the completion menu at two natural typing points:
     *   1) '/' at the start of an empty line → command menu appears
     *      instantly (no TAB needed). Mid-line slashes in URLs /
     *      paths / regex are left alone because xline only fires the
     *      trigger at a token boundary.
     *   2) ' ' right after a known slash-command token → argument
     *      menu appears. E.g. typing `/model ` pops the model list
     *      without a TAB. In prose or after unknown commands the
     *      completer produces zero candidates, so the space stays
     *      silent (no beep, no flash) — adding space to the trigger
     *      set is safe thanks to xline's empty-menu suppression on
     *      auto-trigger. */
    xLineSetCompletionTriggers("/ ");
    /* Default hint delay is 500ms — fine for typing prose where you
     * don't want a flash every keystroke, but annoying when you've
     * just typed `/c` and know `clear` is coming. Zero delay makes
     * the inline hint feel instant, matching fish-shell / zsh-
     * autosuggestions UX. Hints still only appear when the current
     * prefix resolves to a single candidate, so ambiguous prefixes
     * (`/h` — /help vs /history) stay quiet until you type more or
     * hit Tab to see the menu. */
    xLineSetHintDelay(0);

    /* Flush stdout before handing the terminal to xline. Any pending
     * output (the banner above) must clear cooked mode or it can
     * interleave with the prompt paint below. */
    std::fflush(stdout);

    if (repl_open_line(&ctx) != 0) {
      rc = 1;
      goto out_agent;
    }
    repl_open = true;

    xEventLoopSignalWatch(loop, SIGINT, repl_on_sigint, &ctx);
    sig_armed = true;

    xEventLoopRun(loop);
  }

  /* ── Shutdown ladder ─────────────────────────────────────────────
   *
   * Kernel-style forward-only gotos. Every acquire above jumps into
   * the label that frees exactly what's been acquired so far; the
   * happy path falls through from the top and unwinds everything.
   * Keep the order strictly inverse of acquisition. */
  if (sig_armed) xEventLoopSignalWatch(loop, SIGINT, nullptr, nullptr);
  if (repl_open) repl_close_line(&ctx);

  std::printf("\nBye!\n");

out_agent:
  /* No xAgentSessionDestroy needed — the default session is owned
   * by the agent and destroyed automatically in xAgentDestroy. */
  if (agent) xAgentDestroy(agent);
  /* Memory store outlives the agent (contract in xagent/memory.h);
   * destroy it only after every session has been torn down, which
   * xAgentDestroy guarantees above. */
  if (memory_store) xAgentMemoryDestroy(memory_store);
out_tool:
  if (shell_tool) xAgentToolDestroy(shell_tool);
out_cfg:
  /* cli_model_config_destroy releases providers AND the registry in
   * the right order. Must run after xAgentDestroy so no session
   * still holds a provider reference. */
  cli_model_config_destroy(&model_cfg);
out_http:
  xHttpClientDestroy(http);
out_loop:
  xEventLoopDestroy(loop);
  goto out;
out_flagset:
  xFlagSetDestroy(fset);
out:
  return rc;
}
