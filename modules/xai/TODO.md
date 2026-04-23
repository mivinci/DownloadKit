# xai — TODO

Running list of scope items that are **intentionally deferred** past
the first header-only drop. Each entry is meant to be picked up
without touching the public API shipped today.

## Upstream dependencies (must land before / with MVP)

- [x] Add `xErrno_Busy` to `xbase/error.h` — referenced by
      `xAiSessionInput()` to signal "a previous run is still in
      flight". Landed in `5f3a2d2`; xstrerror maps it to "resource
      busy".

## MVP implementation (next batch)

- [x] `message.c` — `xAiContentText` / `xAiMessageFromContent` /
      `xAiMessageFromText` (thread-local one-slot buffer for the
      last one, as documented).
- [x] `tool.c` — `xAiToolCreate` / `xAiToolDestroy`; snapshot of
      caller-owned strings into the tool's own storage.
- [x] `provider.c` — shared `xAiProvider` base struct (vtable +
      impl pointer), plus `xAiProviderDestroy`.
- [x] `provider_openai.c` — OpenAI-compatible provider. Uses
      `xHttpClientDoSse` for the streaming wire protocol, incremental
      JSON parser for choice/content/tool_calls deltas, and coalesces
      fragmented tool-call arguments before dispatching to the session
      layer.
- [x] `agent.c` — bookkeeping for provider / tools / limits / task
      group, no own loop state. Landed in `43ede0a`.
- [~] `session.c` — the actual agent loop:
  - [x] lifecycle (create / destroy / cancel);
  - [x] single-in-flight run enforcement (`xErrno_Busy` on re-entry);
  - [x] history ownership (session duplicates every byte);
  - [x] text-only round: submit view build, text delta streaming,
        provider stop-reason → `xAiDoneReason` translation;
  - [x] partial-text commit on error / cancel;
  - [ ] tool dispatch to the task group with `concurrent_safe`
        gating (tool_use currently terminates the round with
        `xAiDoneReason_ToolError` + on_error diagnostic);
  - [ ] internal Terminal / Continue tagged union — the MVP only
        does one provider round per input, so no state machine is
        needed yet; this lands with the tool loop;
  - [ ] local context-budget trip wire / max_turns / max_tokens
        enforcement (provider's own PromptLong signal is already
        mapped to `xAiDoneReason_PromptTooLong`).
- [ ] End-to-end smoke tests against a local OpenAI-compatible
      endpoint (llama.cpp server or stub).

## Session.c follow-ups (Commit 4 candidates)

- [ ] Tool loop: receive `xAiContentType_ToolUse`, look up the tool
      on the agent, dispatch the handler (synchronously on the loop
      for MVP), append the `tool_result` to history, submit the
      next round.
- [ ] Finalise on_done vs on_error split. Current policy: on_error
      is an informational pre-cursor (tool-not-supported, provider
      transport error) followed by on_done. session.h's "exactly
      one" wording is stricter — decide which side wins and update
      both header + implementation in the same commit.
- [ ] Async teardown: destroying a session while a run is in flight
      today only calls `ai_provider_cancel` and immediately frees
      state. This relies on the provider delivering on_done
      synchronously from cancel(), which provider_openai does not
      promise. Add a drained-by-loop teardown path.

## Provider expansion

- [ ] `provider_anthropic.{h,c}` — Anthropic Messages API.
      Separate header (`<xai/provider_anthropic.h>`) with its own
      `xAiAnthropicConf`; wire-protocol differences handled inside
      the impl so the session layer stays untouched.
- [ ] `provider_local_llamacpp.{h,c}` — direct in-process provider
      for embedded / offline use.
- [ ] Prompt caching hooks (Anthropic `cache_control`, OpenAI
      prompt cache headers) surfaced via per-message flags inside
      the provider, invisible to callers.

## Context management

- [ ] Multi-layer compression pipeline inspired by Claude Code:
  - snip (per-tool-result truncation),
  - microcompact (single-turn squashing),
  - collapse (structural rewrite of older turns),
  - autoCompact (budget-driven summary),
  - reactiveCompact (emergency bail-out on `PromptLong`).
- [ ] Pluggable summariser (use the same provider? a cheaper model?
      a caller-supplied callback?). Stick an interface only when
      the second consumer appears.
- [ ] Token accounting instead of byte accounting for
      `context_budget`.

## Tooling

- [ ] Confirmation flow for `needs_confirm` tools: opt-in callback
      on `xAiSessionConf`, plus `xAiSessionApprove/Reject(call_id)`.
      Add without breaking existing callers by introducing optional
      fields at the end of `xAiSessionConf`.
- [ ] MCP (Model Context Protocol) adapter: thin shim that exposes
      an MCP server's tool catalog as a set of `xAiTool`s. Lives in
      a separate translation unit; no change to `tool.h`.
- [ ] Per-tool cancel token so a long-running tool can be aborted
      when the session is cancelled mid-flight.

## Session semantics

- [ ] Session concurrency / queueing policy: today a second
      `xAiSessionInput()` while a run is active returns
      `xErrno_Busy`. Consider an opt-in queue mode and, if added,
      an `on_queued` callback. Do NOT change the default — we want
      explicit back-pressure.
- [ ] Observer hooks for tracing (per-turn timing, token counts,
      tool latency). Separate `<xai/observer.h>` so the core
      headers stay minimal.
- [ ] Session persistence: serialise / restore message history for
      UIs that resume long-running conversations.

## Build / packaging

- [ ] Replace the placeholder TU in `CMakeLists.txt` once real
      sources land.
- [ ] Decide whether OpenAI SSE + JSON parsing pulls in cJSON as a
      `PRIVATE` dep (xfer already does) or we write a tiny
      purpose-built parser to keep xai self-contained.
- [ ] Headers install rules, once xKit ships a public install
      target.
