# Design

A collection of architecture-level design documents that are not tied to any
single module. These are methodology notes — reusable patterns, cross-cutting
decisions, and design rationale that outlive any individual implementation.

Each document here states a problem shape, proposes a structure, and compares
the structure against the common alternative of _not_ doing it. They are
intended to be readable on their own, without prior knowledge of xKit internals.

## Index

- [Three-Layer Conversation Model](three-layer-conversation-model.md) —
  A way to carve systems that have "long-lived identity + multi-turn session +
  one-shot request" topology into three layers (Agent / Session / Query), and
  what it concretely buys you compared to the one-fat-object default.
- [Context Budget](context-budget.md) —
  How the Session layer keeps outgoing prompts under a token ceiling without
  bleeding history ownership into Provider or Query. Covers the three-piece
  split (estimator / EWMA calibrator / front-trimmer), the policy gate wiring,
and walks through the live numbers printed by `apps/cli`.
- [Layered Memory](layered-memory.md) —
  The four-layer memory / behaviour stack that sits on top of the three-layer
  conversation model: L1 immediate extraction, L2 long-term store & retrieval,
  L3 mood & vitality tracking, L4 proactive wake-up & scheduling. Covers the
  data flow, the per-layer protocols, the three-type session interaction model,
  and the MVP landing sequence.
