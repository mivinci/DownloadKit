# xai — TODO

> **架构级状态（2026-04-25）**：human-like-ai MVP 扳机已扣下，详见
> [`docs/todo/human-like-ai.md`](../../docs/todo/human-like-ai.md) §6。
> Session/Query 拆分 **Step 1 与 Step 2 核心已落地**：`xAiQuery` 已是
> first-class public handle，与 `xAiSession` history 解耦，§8.1/§8.2/§8.3
> 三条 Agent 预留勾子全部就位（observer list、input origin、finalizing
> hook）。`npm test` 9/9 全绿。Step 3（`xAiQueryCreateStandalone`）按
> 架构文档为"可选"，保持原计划仅在有真实用例时再做。
>
> 架构文档 §11.3 里写过的 "fake_submit → fake_query" 改造经评估**不再
> 做**——`session_test.cpp` 当前通过 fake provider 驱动出 Query 的完整
> 执行链，已经等价于"Session + Query 集成测试"；Query 白盒覆盖由新增的
> `query_test.cpp` 独立承担。详见架构文档 §11.3 addendum。
>
> **下一个动手项：§1 Async teardown**（与 Session/Query 拆分 Step 2 一同落地）。
> §7 context_budget α 已完成（四个 commit 全部落地 + EWMA 校准器增强，
> 46 个测试全绿），详见已完成归档。

模块级落地细节清单。**架构级 / 跨层 TODO 不住这里**，看
[`docs/todo/xai_architecture.md`](../../docs/todo/xai_architecture.md)
（三层切分、Session/Query 拆分、Agent 层登记）和
[`docs/todo/human-like-ai.md`](../../docs/todo/human-like-ai.md)
（产品方向 + MVP 执行边界）。

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

## 2. Agent 分层记忆体系（L1–L4）

Agent 层的四层记忆 / 行为体系。L1 钩子已预留（`on_produced` +
`on_finalizing`），L2–L4 待落地。

三种 session 的交互模型：

| 场景 | Origin | 谁创建 | 谁销毁 | 生命周期 |
| ------ | -------- | -------- | -------- | ---------- |
| Default Session | `User` | Agent 创建时自动 | Agent 销毁时自动 | 跟 Agent 同生共死 |
| 用户新开 Session | `User` | 用户调 `xAiAgentCreateSession` | 用户调 `xAiSessionDestroy` | 用户控制 |
| Agent 主动唤醒 Session | `SystemSynthesized` | Agent 内部 | Agent 内部 | Agent 控制 |

### L1 — 即时记忆提取（Immediate Memory Extraction）

- [x] **`on_produced` 钩子**。
  位置：session 每轮 provider 产出后、`on_done` 前。Agent 通过
  `xAiAgentCreateSession` 注入。当前实现为 stub（空回调）。
  提取目标：用户偏好、关键事实、决策记录等结构化观察。

- [x] **`on_finalizing` 钩子**。
  位置：session 销毁前（`xAiSessionDestroy` 内）。Agent 注入。
  当前实现为 stub。提取目标：会话级摘要、情绪 delta、整体印象。

- [ ] **L1 提取逻辑实现**。
  当前两个钩子是空 stub。需要：
  1. 设计提取产物的结构化 schema（偏好 / 事实 / 决策各是什么格式）
  2. 实现 `on_produced` 内的提取逻辑（可能用一次内部 LLM 调用
     "extract observations from this exchange"）
  3. 实现 `on_finalizing` 内的汇总逻辑
  4. 提取结果交付给 L2 持久化

### L2 — 长期记忆存储与检索（Long-term Memory Store & Retrieval）

- [ ] **记忆存储后端**。
  持久化 L1 提取的结构化记忆。设计选项：
  - (a) 简单文件 / SQLite（嵌入式，零依赖）
  - (b) 向量数据库（语义检索，但加重依赖）
  初期倾向 (a)，等真实 workload 再评估 (b)。

- [ ] **记忆注入到新 session**。
  `xAiAgentCreateSession` 创建 session 时，从 L2 检索与当前上下文
  相关的记忆，注入到 system prompt 或 history 前缀。这是用户说的
  "认知、记忆之类的由 agent 在创建的时候注入"。

- [ ] **记忆淘汰 / 合并策略**。
  长期运行后记忆条目会膨胀。需要：
  - 相似记忆合并（"喜欢简洁" + "偏好简短" → 合并）
  - 过期淘汰（时间衰减权重）
  - 容量上限（LRU 或优先级排序）

### L3 — 情绪 / 状态追踪（Mood & State Tracking）

- [ ] **Mood delta 追踪**。
  在 `on_finalizing` 中记录每次会话的情绪变化量：
  - 情绪维度：满意度、困惑度、紧迫感等
  - 累积方式：session 级 delta → agent 级 running average
  - 不需要 LLM 调用，可以从 usage pattern / 对话长度 / tool 调用频率
    等信号量推断

- [ ] **活跃度 / 疲劳度模型**。
  追踪 agent 的"工作状态"：
  - 近 N 小时内的 session 数 / token 消耗量
  - 活跃度评分 → 影响 L4 的唤醒频率（活跃时少打扰，空闲时可以提醒）
  - 疲劳度 → 触发 context budget 更激进的压缩策略

### L4 — 主动唤醒 / 调度（Proactive Wake-up & Scheduling）

- [ ] **定时 / 事件驱动唤醒框架**。
  Agent 自行决策何时主动创建 `origin=SystemSynthesized` 的 session，
  调 `xAiSessionInput` 发起主动对话。场景：
  - 定时提醒（"该复查代码了"）
  - 任务完成通知（后台 tool 执行完毕）
  - 主动建议（"我发现一个优化点"）

- [ ] **唤醒策略**。
  基于状态量决定是否唤醒：
  - L3 的活跃度 / 疲劳度 → 唤醒频率控制
  - L2 的记忆 → 唤醒内容的个性化
  - 外部事件 → tool 返回的 deferred result / 文件变更通知等

- [ ] **唤醒 session 的生命周期管理**。
  - 创建：agent 内部调 `xAiAgentCreateSession`，origin 设为
    `SystemSynthesized`
  - 运行：组装 agent nudge 消息，调 `xAiSessionInput`
  - 销毁：对话结束后 `xAiSessionDestroy`
  - 与 default session 的关系：两者独立，互不干扰。Default session
    是用户的默认入口（origin=User），唤醒 session 是 agent 主动发起
    （origin=SystemSynthesized）。

---

## 3. Provider 扩展

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

## 4. Tool 工程

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

## 5. 测试

- [ ] **E2E smoke test**：起一个 local OpenAI-compatible endpoint
  （llama.cpp server 或自写 stub），跑完整 session → tool → session
  闭环。
  现在只有 unit test 覆盖 provider 解析 + session 状态机，缺少
  "真实 HTTP + 真实流式" 的端到端。
  **动手时机**：Session/Query 拆分的 Step 2 做完时顺手补——拆出
  `xAiQuery` 之后恰好是测"单次 query 端到端"的最小单元。

## 6. 构建 / 打包

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

## 7. Context budget（历史长度管控）

**问题**：一次 query 发起前，messages（system prompt + 历史 +
tool_use/tool_result 对）累加的 token 可能超过模型 context window。
模型侧典型反应是 400 或静默截断，两种都很糟：前者让 session 崩掉，
后者让模型"失忆"但无告警。需要 xai 在 provider 之前把关。

三个触发点本质是同一个：**即将把 messages 交给 provider 前**。我们只
在这一个钩子上统一拦截——不管是 `xAiSessionInput` 刚开的新轮还是
tool_result 回流后的下一轮，都走这里。

### 7.1 分阶段落地

**分两步走，不要一把梭**：α 铺骨架，β 复用 α 的 hook 做真正的智能压缩。

- [x] **α：硬截断骨架（方案 TruncateOldest）**。
  零 LLM 调用、零延迟，把"预算检查 → trim → 保留边界"这条链路打通，
  同时暴露 hook 给 β 复用。
  拆成下列 commit：
  1. `xAiBudgetPolicy` 枚举 + `xAiSessionConf.budget`
     字段定义（默认 disabled，向后兼容）
  2. Token 预估器（按字节数 /4 粗估，见 §7.3）+ "保留边界"工具函数
     （system prompt / 当前 Input() 的新 user 消息 / 未配对的
    tool_use↔tool_result 不能丢，见 §7.4）
  3. Query 发起前的预算检查 + TruncateOldest 策略完整闭环
  4. 单测：覆盖 "边界正好 / 刚好超 / 远超 / tool 对完整性" 四档

- [x] **β：summary query 调度**。
  在 α 的 hook 点上，当策略为 `SummarizeOldest` 时，session 起一次
  **短平快的内部 query**（复用同一 agent 的 provider，但带独立 system
  prompt "Summarise this conversation segment in ≤200 words, preserve
  names, numbers, decisions"），把老消息替换成一条
  `role=System, content="[summary] ..."`。
  - 这个内部 query 必须 **`budget_policy=Disabled`**，否则递归爆栈
  - 压缩期间 session 对外状态是 "busy/compacting"，继续 Input() 依然
    返 `xErrno_Busy`
  - 失败（provider error / summary 自己超 budget）降级为 α 的硬截断，
    不把压缩失败当致命错误
  **动手时机**：α 跑通且有真实长对话 workload 后再做。没有实际 workload
  的 β 等于过度设计。

### 7.2 预算配置位置

放 **`xAiSessionConf`**，不是 agent 也不是 query：

- 不同 session 用途可能不同（正常对话 vs 压缩任务本身）
- 压缩 query 需要能**单独关掉**预算检查（避免递归）→ 这要求 Query 层
  保留一个 override 口子，`xAiQueryConf.budget_policy_override`
  （可选，默认沿用 session）
- Agent 层放预算会让多个 session 共享，但它们用同一个 model 不代表
  用同一个预算（比如一个 session 只用来做 title 生成，预算 512；另一个
  做主对话，预算 16k）

字段草案（写到 `session.h`，不是独立头）：

```c
XDEF_ENUM(xAiBudgetPolicy) {
  xAiBudgetPolicy_Disabled       = 0,  /* 默认：不做任何检查 */
  xAiBudgetPolicy_Error          = 1,  /* 超预算直接 on_error，最安全 */
  xAiBudgetPolicy_TruncateOldest = 2,  /* α 方案：从最旧开始丢 */
  xAiBudgetPolicy_Callback       = 3,  /* 回调 on_compact 让调用方决定 */
  xAiBudgetPolicy_SummarizeOldest= 4,  /* β 方案：起 summary query */
};

XDEF_STRUCT(xAiBudgetConf) {
  xAiBudgetPolicy policy;
  int             max_tokens;        /* 0 = policy 自带默认 */
  int             keep_recent_turns; /* TruncateOldest/Summarize 必保留 */
};
```

### 7.3 Token 预估

三档选择：

- (a) **字节数 /4 粗估**：0 依赖、误差 ±30%，对中文偏乐观
- (b) 引入 tiktoken 或等价 bpe：准，但加依赖、加构建体积
- (c) 依赖 provider 上一次返回的 `prompt_tokens` 做 ex-post 校准

**采纳 (a) + (c)**：

- (a) 先走通，作为基线
- 每次 provider 返回 usage，把 "estimated / actual" 比值记成 session 层
  的一个 rolling calibration factor（滑动平均），下次估算乘上这个因子
- 不碰 (b)：tiktoken 依赖太重，而 (c) 校准后的 (a) 对"触发阈值"够用

实现位置：`session.c` 内 `static size_t estimate_tokens(const xAiMessage *, size_t n)`，不暴露到头文件。

### 7.4 保留边界（硬不变量）

无论策略怎么选，**这四类消息绝不能被 trim 掉**：

1. **System prompt**（首条 `role=System`）—— 丢了模型身份/工具说明全丢
2. **当前 Input() 的新 user 消息** —— 正在处理的用户输入
3. **未配对的 tool_use ↔ tool_result** —— 丢半边 provider 直接 400
   （OpenAI: "tool_call_id not found"；Anthropic: "tool_use block
   without matching tool_result"）。Trim 必须以"完整的
   user+assistant(+tool pair)\*" 为原子单位
4. **最近 `keep_recent_turns` 轮** —— 配置项，默认 2

这块最容易埋 bug，拆一个 `trim_messages_preserving_pairs()` 出来，
单测用 fixture 覆盖：只有 tool_use 没 tool_result、tool_result 跨轮、
连续多个 tool_use 等 case。

### 7.5 回调契约（Callback 策略 / Summarize 策略共用）

```c
/* session.h 追加 */
XDEF_STRUCT(xAiSessionCallbacks) {
  /* ...现有字段... */

  /* 预算超标时触发。Session 已经算出 estimated/budget，让调用方
   * 决定改写后的 messages。返回 Ok=接受改写；非 Ok=放弃本次 Input，
   * session 走 on_error(xErrno_ResourceExhausted)。
   * 改写必须保持 §7.4 的四条不变量，session 会再校验一次。*/
  xErrno (*on_compact)(const xAiMessage *in, size_t n_in,
                       xAiMessage **out, size_t *n_out,
                       size_t estimated_tokens, size_t budget,
                       void *user_data);
};
```

Summarize 策略不暴露 on_compact——它自己内部实现，对用户透明。
Callback 策略给高级用户用（自己接 tiktoken、自己决定丢哪条）。

### 7.6 动手时机

**Step 2（Session/Query 拆分）之后**。原因：Query 层会承接"发起
provider 调用"这个动作，budget hook 装在 Query 里比装在 Session 里
自然。而且 β 方案需要 Session "起一次内部 Query" 的能力——Query
独立出来之后这是 trivial 的一行，现在还要绕 session.c 的内部状态机，
不值得。

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
- Context budget α：`xAiBudgetPolicy` 枚举 / `xAiBudgetConf` 配置 /
  Token 预估器（bytes÷4 + EWMA 校准器）/
  TruncateOldest 裁剪闭环 /
  保留边界（system prompt / 当前 input / tool 对完整性 / keep_recent_turns）/
  46 个测试全绿（budget_test 33 + session_test Budget* 13）

具体细节看 git history 和 `MEMORY.md`。
