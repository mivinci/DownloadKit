/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * main.cpp - xKit command-line entry.
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
 *   ./xkit [-d, --data-dir <path>]                   # default: cwd
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
 *   main.cpp     - this file: argv parsing, object wiring, banner,
 *                  event loop drive, shutdown
 */

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
#include <vector>

#include <xagent/agent.h>
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

/* ── Banner text-wrap helper ──────────────────────────────────────
 *
 * Soft-wrap a pure-ASCII paragraph to a column width for drawing
 * inside the startup banner's bordered box. Break priority:
 *   1. space   — natural word break, preferred
 *   2. '/'     — next-best break point (long POSIX paths have
 *                plenty of these, so a deeply-nested data_dir
 *                doesn't overflow); break kept *after* the slash
 *                so the reader still sees the separator on the
 *                upper line.
 *   3. hard cut at `width` — last-resort fallback when a single
 *                token (e.g. a path component with no slashes)
 *                is longer than the column.
 *
 * Input must be pure ASCII (byte count == display width); the
 * banner's width accounting relies on that and Unicode here would
 * throw off the %-*s padding downstream. Empty input produces one
 * empty line so the caller can still emit a blank row and keep
 * vertical rhythm. */
static std::vector<std::string> banner_wrap(const std::string &text,
                                            size_t             width) {
  std::vector<std::string> out;
  if (width == 0) {
    out.push_back(text);
    return out;
  }
  size_t i = 0, n = text.size();
  while (i < n) {
    /* Remaining text fits on one line — emit and done. */
    if (n - i <= width) {
      out.push_back(text.substr(i));
      break;
    }
    /* Scan the next `width` bytes for the rightmost break point.
     * Prefer space; if none, fall back to the rightmost '/'. */
    size_t brk_space = std::string::npos;
    size_t brk_slash = std::string::npos;
    for (size_t j = 0; j < width; j++) {
      char c = text[i + j];
      if (c == ' ') brk_space = j;
      else if (c == '/') brk_slash = j;
    }
    if (brk_space != std::string::npos) {
      /* Break *at* the space: line ends before it, next line skips
       * the space itself. */
      out.push_back(text.substr(i, brk_space));
      i += brk_space + 1;
    } else if (brk_slash != std::string::npos) {
      /* Break *after* the slash: keep the '/' on the upper line so
       * the path separator is still visible to the reader. */
      out.push_back(text.substr(i, brk_slash + 1));
      i += brk_slash + 1;
    } else {
      /* Single token longer than the column — hard cut. */
      out.push_back(text.substr(i, width));
      i += width;
    }
  }
  return out;
}


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
  xFlagSet      fset       = nullptr;
  xEventLoop    loop       = nullptr;
  xHttpClient   http       = nullptr;
  xAgentTool    shell_tool = nullptr;
  xAgent        agent      = nullptr;
  bool          repl_open  = false;
  bool          sig_armed  = false;

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
  fset = xFlagSetCreate("xkit", "xKit command-line tool");
  if (!fset) {
    std::fprintf(stderr, "failed to create flag set\n");
    rc = 1;
    goto out;
  }
  /* Wire --version / -V to the CMake-injected build version so
   * `xkit --version` stays in lockstep with the banner. xFlagParse
   * prints and returns xErrno_Again (handled below). */
  xFlagSetVersion(fset, XKIT_VERSION);
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
     * 8192 was picked empirically: large enough that a single
     * long-form answer (think: a derivation with multi-paragraph
     * reasoning) plus the floor pinned by keep_recent_turns won't
     * trip the gate on turn #2, but small enough that a handful of
     * sustained turns will eventually push the rolling history
     * past the cap and exercise TruncateOldest. keep_recent_turns
     * =2 is the floor — the current user turn and the immediately
     * prior assistant turn are never discarded, so the model keeps
     * local context even when the trimmer fires. If you shrink
     * max_tokens below ~4096 expect xErrno_PromptTooLong (which the
     * REPL and on_error both surface with a hint line below), and
     * see session.c's keep_recent_turns floor logic for why. */
    sconf.budget.policy            = xAgentBudgetPolicy_Auto;
    sconf.budget.max_tokens        = 8192;
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
      "You are a concise assistant running in xKit's command-line "
      "chat. You have access to a shell tool that can execute "
      "commands via /bin/sh -c and return stdout/stderr/exit code. "
      "Use it when you need to run commands, check the system, or "
      "compute things. You may chain multiple tool calls in a single "
      "turn. Keep replies short.";
    aconf.tools                = tool_ptrs;
    aconf.tools_count          = TOTAL_TOOLS;
    aconf.max_turns            = 64;
    aconf.agent_id             = "test";
    aconf.data_dir             = data_dir;
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
     * prompt. A 72-col bordered box. The body has two shapes:
     *
     *   Happy path (model configured):
     *     Two-column layout — a small ASCII-art "xkit" logo pinned
     *     to the left (slant font, 3 lines) alongside the session
     *     knobs (model id, data_dir) on the right. A one-line tips
     *     strip sits below.
     *
     *   Degraded path (no models.json / empty registry):
     *     Same logo on the left, but the right column is empty.
     *     Below the logo, a yellow warning paragraph explains how
     *     to enable chat, soft-wrapped across as many lines as
     *     needed — a deeply-nested data_dir thus expands the
     *     banner gracefully instead of being truncated.
     *
     * Styling: ANSI bold for the title, faint for the border. Box
     * drawing uses Unicode (any modern terminal; collapses visually
     * on a dumb tty but still prints sensibly).
     *
     * Width discipline: inside the box we rely on the fact that every
     * body line is pure ASCII, so byte count == display width. That
     * lets printf's %-Ns pad to the right │ without manual counting.
     * The layout is
     *
     *   │ <LOGO 17 cols><2 cols gap><RIGHT 49 cols> │
     *
     * so BOX_INNER = 17 + 2 + 49 = 68 and the full frame is 72 cols.
     * `model` and `data_dir` are user-supplied so we truncate them to
     * fit the 49-col right column instead of blowing the frame. */
    enum {
      LOGO_W    = 17,
      GAP_W     = 2,
      RIGHT_W   = 49,
      BOX_INNER = LOGO_W + GAP_W + RIGHT_W, // 68
    };
    char right[RIGHT_W + 1];
    // Leading blank line: the parent shell's prompt sits right above
    // our first row, so without this gap the top border visually
    // collides with `$ xkit` (or whatever PS1 trailed on). One row
    // of breathing room is enough and costs nothing.
    std::printf("\n");
    // Top border is 72 cells: "┌─ " (3) + "xKit " (5) + VERSION + " " (1)
    // + N*"─" + "┐" (1). The product name is hard-coded (not themed via
    // XKIT_NAME or similar) because there's exactly one product and a
    // macro would just be indirection for indirection's sake.
    // XKIT_VERSION is injected by CMake from XK_VERSION in the root
    // CMakeLists.txt so the banner never drifts from the real build.
    {
      const char *ver    = XKIT_VERSION;
      int         ver_w  = (int) std::strlen(ver);
      int         dashes = 72 - 3 - 5 - ver_w - 1 - 1;
      if (dashes < 0) dashes = 0;
      std::printf("\x1b[2m┌─ \x1b[22m\x1b[1mxKit %s\x1b[22m\x1b[2m ", ver);
      for (int i = 0; i < dashes; i++) std::printf("─");
      std::printf("┐\x1b[22m\n");
    }
    // empty top padding row
    std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");

    /* Logo rows paired with the knob lines. The logo is pure ASCII so
     * byte count == display width; each row is exactly LOGO_W cells.
     * The right column shows the *default* (= initial) model resolved
     * from the registry. When the user later runs /model <id> the
     * above_printf in slash_cmd_model echoes the new selection — we
     * don't rewrite the banner itself because it's a one-shot startup
     * print, and scrolling a whole redraw just for one line would
     * fight the line editor. */
    const char *logo0 = "    ___  __ _ __ ";
    const char *logo1 = "   / _ \\/ /(_) /_";
    const char *logo2 = "  /_//_/_//_/\\__/";

    /* Logo + right column. In degraded mode the right column is
     * intentionally blank: model/data_dir carry no actionable info
     * when there's no model to chat with, and the space is better
     * spent on the wrap block below that tells the user what to do.
     * In the happy path we show model id on row 0 and data_dir on
     * row 1; row 2 is a spacer. */
    if (no_models) {
      std::printf("\x1b[2m│\x1b[22m %s%*s%-*s \x1b[2m│\x1b[22m\n", logo0,
                  GAP_W, "", RIGHT_W, "");
      std::printf("\x1b[2m│\x1b[22m %s%*s%-*s \x1b[2m│\x1b[22m\n", logo1,
                  GAP_W, "", RIGHT_W, "");
      std::printf("\x1b[2m│\x1b[22m %s%*s%-*s \x1b[2m│\x1b[22m\n", logo2,
                  GAP_W, "", RIGHT_W, "");
    } else {
      const xAgentModelSpec *dspec = xAgentModelRegistryGet(
        model_cfg.registry, model_cfg.default_id.c_str());
      const char *dmodel = dspec && dspec->model ? dspec->model : "?";
      std::snprintf(right, sizeof(right), "model=%s (id=%s), tools=shell",
                    dmodel, model_cfg.default_id.c_str());
      std::printf("\x1b[2m│\x1b[22m %s%*s%-*s \x1b[2m│\x1b[22m\n", logo0,
                  GAP_W, "", RIGHT_W, right);

      std::snprintf(right, sizeof(right), "data_dir: %s", data_dir);
      std::printf("\x1b[2m│\x1b[22m %s%*s%-*s \x1b[2m│\x1b[22m\n", logo1,
                  GAP_W, "", RIGHT_W, right);

      std::printf("\x1b[2m│\x1b[22m %s%*s%-*s \x1b[2m│\x1b[22m\n", logo2,
                  GAP_W, "", RIGHT_W, "");
    }

    // blank separator before the degraded-mode hint / tips strip
    std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");

    /* Degraded-mode hint block.
     *
     * Full-width wrapped paragraph placed below the logo rather
     * than compressed into the 49-col right column. A 2-col left
     * indent (matching the tips strip and logo inset) anchors the
     * block visually; the wrap width is therefore BOX_INNER - 2 so
     * the indent sits *inside* the printed field and every line —
     * first and continuations alike — aligns under the same
     * column. Each line is printed with the yellow attribute so
     * the block reads as a single advisory, with the "[!]" marker
     * on the first line anchoring it visually. */
    if (no_models) {
      char hint_buf[4096];
      std::snprintf(hint_buf, sizeof(hint_buf),
                    "[!] no model is configured, edit %s/models.json "
                    "to enable chat",
                    data_dir);
      const size_t indent = 2;
      auto hint_lines = banner_wrap(hint_buf, BOX_INNER - indent);
      char padded[BOX_INNER + 1];
      for (const auto &ln : hint_lines) {
        /* Prepend the shared indent by hand so %-*s pads the
         * indent+text as one unit to BOX_INNER. */
        std::snprintf(padded, sizeof(padded), "  %s", ln.c_str());
        std::printf(
          "\x1b[2m│\x1b[22m \x1b[33m%-*s\x1b[39m \x1b[2m│\x1b[22m\n",
          BOX_INNER, padded);
      }
      // spacer between hint block and tips strip
      std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
    }
    // one-line tips strip (indent 2 cols to match logo inset)
    std::printf(
      "\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
      "  Enter send   / commands   Ctrl-C cancel/exit   /help more");
    // empty bottom padding row
    std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
    // bottom: '└' + 70 '─' + '┘' = 72
    std::printf(
      "\x1b[2m└"
      "──────────────────────────────────────────────────────────────────────"
      "┘\x1b[22m\n\n");

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
