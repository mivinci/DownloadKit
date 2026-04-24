# xai — TODO

模块级落地细节清单。**架构级 / 跨层 TODO 不住这里**，看
[`docs/todo/xai_architecture.md`](../../docs/todo/xai_architecture.md)
（三层切分、Session/Query 拆分、Agent 层登记）和
[`docs/todo/human-like-ai.md`](../../docs/todo/human-like-ai.md)
（产品方向）。

本文件只管 provider 扩展、tool 工程、session 语义收尾、构建打包这类
**模块内工程细节**——这些东西无论架构怎么切，该做的还得做。

---

## 1. Session 语义收尾

这几条在做 `docs/todo/xai_architecture.md` §10 Step 1（纯物理重组）时
就得顺手碰——因为它们本来就是现有 `session.c` 的未收敛处，拆文件前
先把语义钉死能省掉 Step 2 的歧义。

- [x] **`on_done` vs `on_error` 的语义钉死**（方案 A）。
  落地：`session.h` 注释改为"on_error 是 diagnostic precursor、on_done
  是 authoritative terminator，每次 accepted Input() 精确触发一次
  on_done"；现状实现已经满足，只改文档。`session_test.cpp` 的
  ProviderError 用例加契约注释，显式断言两者都 fire 一次。
  代码零改动——三处 fire on_error 的路径（provider transport error /
  dispatch 致命错误 / submit follow-up 失败）后面都跟 `finish_run`，
  现状就是方案 A。

- [ ] **Async teardown**。
  现状：`xAiSessionDestroy` 在 run 进行中调用时，只 `ai_provider_cancel`
  然后立刻 free state。这依赖 provider 在 cancel 里**同步**触发 on_done，
  但 `provider_openai` 并没有这个承诺（libcurl easy handle 的关闭路径
  有可能延后到下一 tick）。
  需要一条 drained-by-loop 的 teardown：
  1. `Destroy` 标记 `pending_destroy=1`，发 cancel
  2. on_done 回调里检测到 `pending_destroy`，再走真正的 free 路径
  3. 如果 loop 已经停了、永远不会再触发 on_done，走 force-free fallback
  **动手时机**：Step 2 引入 `xAiQuery` 时一起做——`xAiQueryDestroy` 本来
  也要面对同一个问题，一次改完。

- [ ] **`needs_confirm` 审批流**。
  Tool 结构体上有 `needs_confirm` 字段但 session 层完全没用。
  设计：
  - `xAiSessionConf` 末尾加可选 `on_tool_confirm` callback
  - 触发时 session 暂停 tool dispatch，等调用方 `xAiSessionApprove(call_id)`
    或 `xAiSessionReject(call_id, reason)`
  - Reject 时按"handler 返 error"走，同样回喂 `is_error=1` tool_result
  **注意**：API 演进要用尾部增字段的方式，别改破坏现有 caller。
  **动手时机**：独立小 PR，任何时候都可以做。

- [ ] **Session 并发 / 排队策略**。
  现状：run 进行中再次 `xAiSessionInput()` 返 `xErrno_Busy`。
  长期考虑：opt-in queue mode + `on_queued` 回调——但**默认保持
  back-pressure 不变**，这个不能改。
  **动手时机**：真有两个 caller 场景后再做，现在纯投机。

## 2. Provider 扩展

- [ ] **`provider_anthropic.{h,c}`** — Anthropic Messages API。
  - 独立头文件 `<xai/provider_anthropic.h>` + 自己的 `xAiAnthropicConf`
  - 关键差异在 provider 实现里吞掉：
    - 消息格式 `role: "user" | "assistant"` + `content: [{type, ...}]`
      数组（OpenAI 是 string 或 parts 数组混用）
    - Tool use / tool result 的 block 结构（Anthropic 是 `tool_use`
      block + `tool_result` block；OpenAI 是 `tool_calls` 字段）
    - **thinking blocks**——Anthropic 原生支持，而我们在 `provider_openai.c`
      里已经按 Anthropic 语义实现了 reasoning_content 映射
      （见 MEMORY.md），两边对齐应该比较顺
    - SSE 事件名不同（`content_block_delta` 等）
  - Session 层**不动**——这是三层抽象的检验关口
  **动手时机**：Session/Query 拆完（Step 2 落地）之后再做——当前 provider
  API 会在拆分中微调（usage 透传契约已经稳了，但 thinking delta 的回调
  可能还要动）。

- [ ] **`provider_local_llamacpp.{h,c}`** — 进程内直连 provider。
  - 不走 HTTP，直接 link llama.cpp 或通过 IPC
  - 主要价值：离线 / 嵌入式场景、测试用
  - 技术难点：llama.cpp 自己有事件循环逻辑，跟 `xEventLoop` 如何共存
  **动手时机**：有实际需求再做，目前纯愿望清单。

- [ ] **Prompt caching**。
  - Anthropic `cache_control: {type: "ephemeral"}`
  - OpenAI prompt cache 是隐式的（按前缀自动命中），但有些 endpoint 支持
    显式 header
  - 作为**每条 message 的可选 flag**暴露到 provider 层，session 层不参与
  - Caller 决定哪些前缀值得 cache（通常是 system prompt + tool
    catalog + 稳定的 few-shot）
  **动手时机**：Anthropic provider 落地后自然触发。

## 3. Tool 工程

- [ ] **MCP (Model Context Protocol) adapter**。
  - 薄薄一层 shim：把一个 MCP server 暴露的 tool catalog 翻译成
    一组 `xAiTool`
  - 独立 TU（`tool_mcp.c`？），不动 `tool.h`
  - MCP server 可以是 stdio 子进程或 HTTP endpoint，前者更常见
  - 关键设计：tool handler 里透明转发到 MCP 的 `tools/call`，错误映射到
    `is_error=1` tool_result
  **动手时机**：当我们需要快速接入外部工具生态（filesystem / browser /
  shell 等）时做，目前优先级低于 provider 扩展。

- [ ] **Per-tool cancel token**。
  现状：session 被 cancel 时，正在跑的 tool handler 没法被中断——handler
  是同步函数，要么已返回要么还在卡。
  设计：
  - `xAiToolHandler` 增加一个 `xAiCancelToken` 参数（或通过 user_data
    间接拿到）
  - Handler 内部长任务（HTTP、文件 I/O）定期 check token
  - Session cancel 时 set token，handler 下一次 check 返回
    "aborted"，session 按"handler error"回流
  **动手时机**：有真实长跑 tool 时再做。现在 tool 都是毫秒级同步计算。

## 4. 测试

- [ ] **E2E smoke test**：起一个 local OpenAI-compatible endpoint
  （llama.cpp server 或自写 stub），跑完整 session → tool → session
  闭环。
  现在只有 unit test 覆盖 provider 解析 + session 状态机，缺少
  "真实 HTTP + 真实流式" 的端到端。
  **动手时机**：Session/Query 拆分的 Step 2 做完时顺手补——拆出
  `xAiQuery` 之后恰好是测"单次 query 端到端"的最小单元。

## 5. 构建 / 打包

- [ ] **cJSON vs 手写 parser 的最终抉择**。
  现状：`provider_openai.c` 用 cJSON。xfer 已经 PRIVATE link 了 cJSON，
  复用它零成本。但 xai 将来作为独立 consumer（比如只想用 session 层、
  自己写 provider）时不希望被 cJSON 绑架。
  决策倾向：**保持 cJSON PRIVATE link，不动**——手写 JSON parser 的
  收益撑不起维护成本，而 PRIVATE link 本来就不传染给下游。
  这条留在 TODO 里是为了"记录已考虑过"，不是"待办"。

- [ ] **Headers install 规则**。
  等 xKit 顶层 `install` target 定下来后，xai 的公共头按规则补上
  （`<xai/session.h>` / `<xai/provider.h>` / `<xai/message.h>` 等）。

---

## 已完成（归档）

Git log 里能查到，这里只留大颗粒里程碑作为阅读指引：

- MVP 落地：`message.c` / `tool.c` / `provider.c` / `provider_openai.c`
  / `agent.c` / `session.c`
- Tool loop：同步 dispatch、unknown tool / handler error 走 `is_error=1`
  tool_result 回喂、`max_turns` 硬门限
- Thinking blocks：`reasoning_content` 按 Anthropic 语义映射（provider
  解析 + Assistant 消息序列化双向，详见 MEMORY.md）
- Usage 透传契约：`xAiUsage{prompt,completion,total}` + `-1` 哨兵，
  跨轮累加在 session 层（详见 MEMORY.md）
- `xErrno_Busy` 映射到 xstrerror "resource busy"

具体细节看 git history 和 `MEMORY.md`。
