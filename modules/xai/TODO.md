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

- [ ] `message.c` — `xAiContentText` / `xAiMessageFromContent` /
      `xAiMessageFromText` (thread-local one-slot buffer for the
      last one, as documented).
- [ ] `tool.c` — `xAiToolCreate` / `xAiToolDestroy`; snapshot of
      caller-owned strings into the tool's own storage.
- [ ] `provider.c` — shared `xAiProvider` base struct (vtable +
      impl pointer), plus `xAiProviderDestroy`.
- [ ] `provider_openai.c` — OpenAI-compatible provider. Uses
      `xHttpClientDoSse` for the streaming wire protocol, incremental
      JSON parser for choice/content/tool_calls deltas, and coalesces
      fragmented tool-call arguments before dispatching to the session
      layer.
- [ ] `agent.c` — bookkeeping for provider / tools / limits / task
      group, no own loop state.
- [ ] `session.c` — the actual agent loop:
  - internal Terminal / Continue tagged union (do not leak to the
    public header), mapped to `xAiDoneReason` at the boundary;
  - single-in-flight run enforcement (`xErrno_Busy` on re-entry);
  - tool dispatch to the task group with `concurrent_safe` gating;
  - history management and context-budget trip wire (coarse
    truncation for MVP; see "Context management" below).
- [ ] End-to-end smoke tests against a local OpenAI-compatible
      endpoint (llama.cpp server or stub).

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
