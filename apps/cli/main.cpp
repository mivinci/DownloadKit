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
 *   export LLM_API_URL="https://api.openai.com/v1"   # optional, no
 *                                                    # trailing slash
 *   export LLM_API_KEY="sk-xxx"
 *   export LLM_MODEL="gpt-4o"                        # optional
 *   ./cli [-d <path>]                                # default: cwd
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
#include "ctx.h"
#include "output.h"
#include "repl.h"
#include "slash.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>

#include <xagent/agent.h>
#include <xagent/provider.h>
#include <xagent/provider_openai.h>
#include <xagent/session.h>
#include <xagent/tool.h>
#include <xagent/tool_shell.h>
#include <xbase/backtrace.h>
#include <xbase/event.h>
#include <xhttp/client.h>
#include <xline/line.h>

int main(int argc, char *argv[]) {
  xPrintBacktraceOnCrash();

  /* ── Parse command-line options ─────────────────────────────────── */
  int         opt;
  const char *data_dir_arg = nullptr;
  while ((opt = getopt(argc, argv, "d:h")) != -1) {
    switch (opt) {
    case 'd':
      data_dir_arg = optarg;
      break;
    case 'h':
    default:
      std::fprintf(stderr, "Usage: %s [-d <path>]\n", argv[0]);
      return 1;
    }
  }

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

  const char *api_url = std::getenv("LLM_API_URL");
  const char *api_key = std::getenv("LLM_API_KEY");
  const char *model   = std::getenv("LLM_MODEL");

  if (!api_key) {
    std::fprintf(stderr, "Please set at least LLM_API_KEY:\n"
                         "  export LLM_API_KEY=\"sk-xxx\"\n"
                         "  export LLM_API_URL=\"https://api.openai.com/v1\"  "
                         "(optional)\n"
                         "  export LLM_MODEL=\"gpt-4o\"                       "
                         "(optional)\n");
    return 1;
  }
  if (!model || model[0] == '\0') model = "gpt-4o";

  /* ── Event loop + HTTP client ───────────────────────────────────── */
  xEventLoop loop = xEventLoopCreate();
  if (!loop) {
    std::fprintf(stderr, "failed to create event loop\n");
    return 1;
  }
  xHttpClient http = xHttpClientCreate(loop, nullptr);
  if (!http) {
    std::fprintf(stderr, "failed to create http client\n");
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Provider ───────────────────────────────────────────────────── */
  xAgentOpenAIConf pconf;
  std::memset(&pconf, 0, sizeof(pconf));
  pconf.api_key       = api_key;
  pconf.base_url      = api_url;
  pconf.default_model = model;
  pconf.timeout_ms    = 60000;

  xAgentProvider pvd = xAgentProviderOpenAICreate(loop, http, &pconf);
  if (!pvd) {
    std::fprintf(stderr, "failed to create OpenAI provider\n");
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* ── Tools ─────────────────────────────────────────────────────────── */

  /* Declare the REPL context up front so the shell tool's callbacks
   * can capture a pointer to it. Fields are populated further below
   * as the objects they reference come into existence. */
  ReplCtx ctx;
  ctx.loop = loop;

  xAgentShellConf shell_conf;
  std::memset(&shell_conf, 0, sizeof(shell_conf));
  shell_conf.callback_ud = &ctx;
  shell_conf.on_command  = [](const char *command, const char *cwd, void *ud) {
    auto *c = static_cast<ReplCtx *>(ud);
    if (cwd && cwd[0]) {
      above_printf(c->line, "\x1b[2m  $ (cd %s && %s)\x1b[0m", cwd, command);
    } else {
      above_printf(c->line, "\x1b[2m  $ %s\x1b[0m", command);
    }
  };
  shell_conf.on_result = [](int exit_code, size_t stdout_len, size_t stderr_len,
                            int timed_out, void *ud) {
    auto *c = static_cast<ReplCtx *>(ud);
    above_printf(c->line, "\x1b[2m  exit=%d stdout=%zu stderr=%zu%s\x1b[0m",
                 exit_code, stdout_len, stderr_len,
                 timed_out ? " (timed out)" : "");
  };
  xAgentTool shell_tool = xAgentToolShellCreate(loop, &shell_conf);
  if (!shell_tool) {
    std::fprintf(stderr, "failed to create shell tool\n");
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  const xAgentTool *tool_ptrs[] = {&shell_tool};
  const size_t      TOTAL_TOOLS = 1;
  /* ── Session config (agent's default session) ──────────────────────
   *
   * Instead of creating a session manually and managing its
   * lifecycle, we set default_session_conf on the agent so it
   * creates a built-in default session at construction time.
   * The session is retrieved via xAgentDefaultSession() and
   * is destroyed automatically by xAgentDestroy(). */

  xAgentSessionConf sconf;
  std::memset(&sconf, 0, sizeof(sconf));
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

  /* ── Agent ──────────────────────────────────────────────────────── */
  xAgentConf aconf;
  std::memset(&aconf, 0, sizeof(aconf));
  aconf.loop     = loop;
  aconf.provider = pvd;
  aconf.model    = model;
  aconf.system_prompt =
    "You are a concise assistant running on xKit's xagent session "
    "demo. You have access to a shell tool that can execute "
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

  xAgent agent = xAgentCreate(&aconf);
  if (!agent) {
    std::fprintf(stderr, "failed to create agent\n");
    xAgentToolDestroy(shell_tool);
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  /* Retrieve the agent's built-in default session — no manual
   * create/destroy needed. The session lives for the agent's
   * entire lifetime. */
  xAgentSession sess = xAgentDefaultSession(agent);
  if (!sess) {
    std::fprintf(stderr, "agent has no default session\n");
    xAgentDestroy(agent);
    xAgentToolDestroy(shell_tool);
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  ctx.sess = sess;

  /* ── Startup banner ───────────────────────────────────────────────
   *
   * Printed once in cooked mode before repl_open_line paints the
   * prompt. A 60-col bordered box: the top bar carries the demo
   * title, the body lists the knobs that actually vary between runs
   * (model, tool list, data_dir) and a Tips block with the keys /
   * commands a first-time user needs. Everything else lives behind
   * /help so the banner doesn't grow with every new command.
   *
   * Styling: ANSI bold for the title, faint for the border. Box
   * drawing uses Unicode (any modern terminal; collapses visually
   * on a dumb tty but still prints sensibly).
   *
   * Width discipline: inside the box we rely on the fact that every
   * body line is pure ASCII, so byte count == display width. That
   * lets printf's %-Ns pad to the right │ without manual counting.
   * `model` and `data_dir` are user-supplied so we truncate them to
   * fit the inner 56-col budget instead of blowing the frame. */
  enum {
    BOX_INNER = 56
  }; // visible cols between "│ " and " │"
  char line[BOX_INNER + 1];
  // top: "┌─ AI Agent Core Demo " is 22 cells; + 37 '─' + '┐' = 60
  std::printf("\x1b[2m┌─ \x1b[22m\x1b[1mAI Agent Core Demo\x1b[22m"
              "\x1b[2m ─────────────────────────────────────┐\x1b[22m\n");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  std::snprintf(line, sizeof(line), "model=%s, tools=shell", model);
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, line);
  std::snprintf(line, sizeof(line), "data_dir: %s", data_dir);
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, line);
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "- Enter       send message");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "- /           browse slash commands");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "- /help       show all commands");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "- /cancel     interrupt a running AI call");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "- Ctrl-C      cancel current run / exit when idle");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER,
              "- Ctrl-D      exit on empty line");
  std::printf("\x1b[2m│\x1b[22m %-*s \x1b[2m│\x1b[22m\n", BOX_INNER, "");
  // bottom: '└' + 58 '─' + '┘' = 60
  std::printf("\x1b[2m└───────────────────────────────────────────────────"
              "───────┘\x1b[22m\n\n");

  /* ── Line editor (xline) ──────────────────────────────────────────
   *
   * Persist history under the agent's data_dir so each agent
   * namespace keeps its own recall buffer. 1000 entries is plenty
   * for a chat REPL — isocline prunes oldest on overflow. Passing
   * NULL would disable persistence (history kept only in-memory);
   * passing a path enables both load-on-start and save-on-exit. */
  char hist_path[4096];
  std::snprintf(hist_path, sizeof(hist_path), "%s/.ai_session_history",
                data_dir);
  xLineSetHistory(hist_path, 1000);
  ctx.hist_path = hist_path;

  /* Slash-command completion. isocline calls slash_completer on Tab;
   * when the current prefix resolves to a single match it ALSO shows
   * the tail as an inline faint hint (fish-shell style) without
   * needing Tab. Hints are on by default, but we set them explicitly
   * so a future xLineEnableHint(false) somewhere else doesn't silently
   * break the UX. completion_preview (the faint candidate shown while
   * the menu is open) is already on by default; left as-is. */
  slash_install_completer();
  xLineEnableHint(true);
  /* Auto-open the slash-command menu the instant the user types '/'
   * on an empty line. Saves a TAB keystroke for the most common
   * command-discovery path; the menu still respects arrow keys /
   * ESC / normal character input, so it's a strict UX upgrade over
   * "type '/' then TAB". Only fires when '/' is the first byte of
   * the input — mid-line slashes (paths, URLs, regex) are left
   * alone. */
  xLineSetCompletionTriggers("/");
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
    xAgentDestroy(agent);
    for (size_t i = 0; i < TOTAL_TOOLS; ++i)
      xAgentToolDestroy(*tool_ptrs[i]);
    xAgentProviderDestroy(pvd);
    xHttpClientDestroy(http);
    xEventLoopDestroy(loop);
    return 1;
  }

  xEventLoopSignalWatch(loop, SIGINT, repl_on_sigint, &ctx);

  xEventLoopRun(loop);

  xEventLoopSignalWatch(loop, SIGINT, nullptr, nullptr);
  repl_close_line(&ctx);

  std::printf("\nBye!\n");

  /* No xAgentSessionDestroy needed — the default session is owned
   * by the agent and destroyed automatically in xAgentDestroy. */
  xAgentDestroy(agent);
  for (size_t i = 0; i < TOTAL_TOOLS; ++i)
    xAgentToolDestroy(*tool_ptrs[i]);
  xAgentProviderDestroy(pvd);
  xHttpClientDestroy(http);
  xEventLoopDestroy(loop);
  return 0;
}
