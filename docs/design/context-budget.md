# 上下文预算：xAgentSession 的中段摘要 pipeline

> 一套在不改 Provider、不改 Query、不侵入业务代码的前提下，给 `xAgentSession`
> 加上「prompt 太长怎么办」能力的方案。
>
> 本文面向已经熟悉 moo 三层会话模型（[Agent / Session / Query](three-layer-conversation-model.md)）
> 的读者，描述当前 Session 层 budget pipeline 的形状、四件套各自负责什么、
> 以及为什么我们最终选了「中段摘要 + cache-friendly Query」这条路。

---

## TL;DR

每次 `xAgentSessionInput` 都会经过下面这条流水线：

```text
  incoming user msg
        │
        ▼
  ① Gate check ────────── fits in context_window?  ── yes ─► proceed
        │ no
        ▼
  ② Retroactive trim ──── shrink consumed tool_results in-place
        │ still over?
        ▼
  ③ Summarize ───── splice [head .. recent) → [summary]
        │ failed (provider error / empty)
        ▼
  ④ degrade TruncateTail  drop tail entries to fit
```

一句话：**先省（trim tool_results），再压（summarise middle），实在不行再砍尾（truncate）**。
所有路径都保留 head 前缀和 recent 尾巴 —— 前者为 provider prompt cache 续命，
后者保住当前对话状态。

四件套的代码切分：

| 件 | 文件 | 职责 | 状态 |
| --- | --- | --- | --- |
| 估算器 | `budget.c :: ai_budget_estimate_tokens` | history 大概多少 token | 无状态 |
| 增量记账 | `session.c :: known_prompt_tokens + delta_entries` | 用 provider 上次报的 prompt_tokens 做精确基线，只对新增 entry 估增量 | Session 级 |
| 双向锚点 | `budget.c :: ai_budget_head_band_end` / `ai_budget_recent_band_start` | 给出「头保 K 轮 / 尾保 K 轮」的下标 | 无状态 |
| 闸门 | `session.c :: session_enforce_budget_` | 把上面缝起来，按 `xAgentBudgetPolicy` 决定放行 / 修剪 / 压缩 / 拒绝 | 复用 Session history |

默认 (`xAgentBudgetPolicy_Disabled`) 是字节级 no-op。
推荐配置是 `xAgentBudgetPolicy_Summarize`。

---

## 为什么放在 Session 层

闸门要同时满足三个条件：① 拥有完整 rolling history；② 活得比单次 Query 长；
③ 在 Query 发起之前能做决定。三层里只有 Session 同时满足，所以这是被拓扑
逼出来的选择，不是偏好。Provider 层做要让 history ownership 跨层分裂，
Query 层做活得不够久没法承诺「这次裁掉的内容下次也不再提交」。

---

## 阶段 ①：Gate check（增量记账）

**目标**：判断 `serialized(system + history + incoming) <= context_window`，
做错代价最大的就是这一步 —— 估高了浪费窗口，估低了直接被 provider 401。

### 不做精确 tokenizer

精确 BPE 把 Session 和具体模型绑死（Claude / GPT-4o / Qwen 各家不同），
模型一换就要跟着升级。我们走「粗估 + 精确基线」组合：

- 粗估公式（`budget.c`）：`tokens ≈ Σ(payload_bytes)/4 + n_entries × per_msg_envelope`。
  常量都在 `budget_private.h`，源自 OpenAI cookbook 和 Anthropic 公开建议。
  CJK 会高估、紧凑 JSON 会略低估，但 ±20% 量级，对窗口决策足够用。
- 精确基线：每次 Query 结束后，记下 provider 报告的**首轮** `prompt_tokens`
  到 `known_prompt_tokens`，以及该报告对应的 history 长度 `known_prefix_len`。
  下次 gate 决策时只对 `[known_prefix_len .. now)` 这段新增 entry 做粗估，
  加上 `known_prompt_tokens` 即得当前估算。

### 为什么用首轮 prompt_tokens

`Query.usage.prompt_tokens` 跨多轮工具循环是**累加**报上来的（见
`query.c :: usage_accumulate`）—— 多轮工具对话可以膨胀好几倍，
和单次 submit 的 prompt 大小完全不对应。`xAgentQuery_::first_round_prompt_tokens`
只记首轮值，正好对应 gate 当时下决定时看到的那份 history，归因清晰。

### 为什么不留 EWMA 校准器

旧实现有一个 EWMA 校准器，把 `actual / estimated` 平滑成 `factor` 反过来
缩放估算。新实现直接用 `known_prompt_tokens` 锚住基线，估算只发生在
「上次 provider 报告之后新加的几条 entry」上，这点小窗口里粗估的系统性偏差
小得可以忽略，校准器收益边际接近零，留着只是多一条状态、多一种 corner case
要测，所以删了。

---

## 阶段 ②：Retroactive tool_results trim

**目标**：在 history 还没溢出但已经接近窗口时，先做最便宜的瘦身 ——
把已经被消费过的 `tool_result` 输出原地砍掉，留个 marker。

### 为什么这一步先做

工具输出最容易膨胀（一次 `cat large.log`、一次 `curl -i` 就上万 token），
但模型对它们的依赖通常**只持续到下一条 assistant 消息**：assistant 已经
基于这次工具结果做了推理决策（要么继续调用工具、要么回复用户），
原始输出再保留对未来推理几乎无增量价值。

把 `[tool_result] 一万行 stdout` 替换成 `[result trimmed: was 12345 bytes]`
能瞬间释放大块 budget，而且**不动 history 结构** —— `tool_use ↔ tool_result`
原子对依然完整、prefix 依然有效、prompt cache 依然命中。

### 触发条件

`trim_tool_results_threshold`（token 数，默认 0 = 关）。设到 140000 就是
「估算 ≥ 140k tokens 时启动 retroactive trim」。

只裁「已被消费」的 tool_result —— 即它后面已经有至少一条 Assistant entry。
当前正在等待回复的那次 tool_result 不动（模型没看过结果，砍了等于毁掉这次工具调用）。

### 事件

成功瘦身后 fire `xAgentBudgetEvent_ToolResultsTrimmed`，info 里给出 entries
数量和释放的字节数。失败 / 没触发都不发事件。

---

## 阶段 ③：Summarize（中段摘要）

如果 trim 后还是过线，进入压缩阶段。这是这次重设计的核心。

### 关键决策：摘中段，保两端

旧实现是「保最近 N 轮 + 把前面全部摘要掉」。问题：head 段的任务目标 /
原始用户意图 / 关键约束被摘没了，模型会逐渐「忘了最初要干什么」，
长对话里非常明显。

新实现：

```text
  history = [ U0  A0  U1  A1  U2  A2  U3  A3  U4  A4 ]
              └────head────┘ └─────middle─────┘ └─tail─┘
              keep_head_turns=2                 keep_recent_turns=2
                                ▲
                                head_idx                  recent_idx
                                = 4                       = 8

  压缩后:
  history = [ U0  A0  U1  A1  [summary]  U4  A4 ]
```

- **head**：原始任务目标 / 用户意图 / 关键约束。前 `keep_head_turns` 个
  user turn 字节级保留。
- **middle**：可压缩段。被 splice 成单条 `[summary]` System entry。
- **tail**：当前工作上下文 / 最新工具状态。后 `keep_recent_turns` 个
  user turn 字节级保留。

「user turn」= 1 个 `User` 消息 + 它后面所有 `Assistant` / `Tool` 消息（直到下一个 `User`）。
切割只发生在 `User` 边界 —— 这一条几何约束自动保证 `tool_use ↔ tool_result`
原子对永远不会被拆散，不需要运行时检查。

### Compact Query 的构造（cache-friendly）

发给 provider 摘要的那次 Query：

```text
  messages = history[0 .. recent_idx)            // head + middle，原样
  + ephemeral instruction:
    "Summarise the conversation above in a single concise paragraph.
     Preserve concrete identifiers, decisions, and user intent. ..."
```

要点：

1. **不**重新组织上下文、**不**抽取片段拼成新 prompt。整个 head + middle
   原样发出去 —— provider 看到的 prefix 和上一次主对话**完全一致**，
   prompt cache 100% 命中，摘要这次请求几乎不增加成本。
2. instruction 用英文写。原因是几乎所有公开 LLM 在英文 instruction
   following 上都比 CJK 强一截，摘要这种「跨语言压缩」任务用英文给指令、
   让模型自适应原文语言输出最稳。
3. **不**指明摘要范围。让模型对它看到的全部上下文做整体压缩。
   就算 head 段也被复述一遍也没关系 —— 反正摘要结果只替换 middle 段，
   head 在 history 里仍然字节级保留，模型实际看到的是「head 原文 +
   summary（包含 head 复述）+ tail」，是冗余但绝对安全。

### Splice 时机

Compact Query 异步 round-trip。整个过程：

```text
  T0  user input → gate fail → fire Compacting → return Busy
                   pin head_idx, recent_idx 到 session 私有态
                   发起 compact Query
  T1  compact 完成 → sess_fwd_on_done compact_ok 分支
                     ├─ xArrayRemoveRange(history, head_idx, recent_idx - head_idx)
                     ├─ xArrayInsert(history, head_idx, summary_entry)
                     ├─ persisted_prefix 三种分支就地调整
                     ├─ L1 preserve fire 仅 [head_idx, recent_idx) middle 段
                     └─ fire CompactDone(summary_ok=true)
  T2  caller 看到 CompactDone 事件，重发 user input
```

Caller 看到的入口语义是 `xErrno_Busy` —— 和「上一个 Query 还在跑」同样的
忙码。事件回调 (`on_budget_event`) 是区分「Busy because compacting」vs
「Busy because user query in flight」的唯一渠道。

### persisted_prefix 处理

L1 store 里已经持久化过的前缀长度（`persisted_prefix`），跨 splice 后要重新对齐：

| 原 `persisted_prefix` 位置 | 调整 |
| --- | --- |
| `< head_idx` | 不变（前缀完全在保留 head 内）|
| `[head_idx, recent_idx)` | 钉到 `head_idx + 1`（穿过被摘 middle，跟到 summary 之后）|
| `>= recent_idx` | 减去 `(recent_idx - head_idx) - 1`（middle 段被压成 1 条，整体往前挪）|

### 失败不降级

旧实现 compact 失败会自动降级到 truncate。新实现**不降级**：
fire `CompactDone(summary_ok=false)`，history 完全不动，errno 直接交给 caller。

理由：摘要失败的失败模式（OOM / provider 抽风 / 空输出）通常**不会自愈**，
默默降级 truncate 会让 caller 错过修问题的窗口（可能就是 API key 失效之类的
基础故障）。直接报错让上层决定是否重试 / 切换 provider / 拓宽窗口 / 换提示词，
比偷偷做事可靠。

### `keep_head_turns == 0` 退化

`keep_head_turns = 0` 时 `head_idx = 0`，middle 退化为整个 prefix，
等价旧的「front-replace」语义。老 caller 不改一行代码、行为完全不变。

---

## 阶段 ④：TruncateTail（最后兜底）

兜底用，不推荐做主策略。砍 history 尾部最新的 entry 直到 fit。

为什么砍尾不砍头：保 head 前缀以保 prompt cache 命中。被砍掉的 tail entry
会通过 L1 preserve 回调 (`xAgentL1PreserveReason_Truncated`) 喂给 L1 store，
不会真丢数据 —— 只是从短期工作记忆里挪走。

事件 `xAgentBudgetEvent_Truncated` 报告砍了多少条。

---

## L1 preserve 回调

唯一的「写 L1 持久化层」通道。在三种 budget 事件触发时调用：

| Reason | 触发点 | 喂进去什么 |
| --- | --- | --- |
| `Compacted` | Summarize 成功 splice 后 | 被替换的 middle 段 `[head_idx, recent_idx)` |
| `Truncated` | TruncateTail 砍尾后 | 被砍掉的尾部 entry |
| `Finalizing` | Session destroy 前 | 整个 history（让 L1 收尾）|

**关键不变量**：L1 preserve 回调中喂进去的 entry，**必须**是 history 里
即将消失的那部分 —— 不能多、不能少。多了 L1 会重复存（同一条 entry 既在
history 又在 L1），少了 L1 会漏掉。这条不变量由
`L1PreserveCompactedDeliversMiddleBandOnly` 测试守住。

---

## 配置速查

```c
xAgentBudgetConf b = {};
b.policy                      = xAgentBudgetPolicy_Summarize;
b.context_window              = 128000;   // 模型窗口
b.keep_head_turns             = 2;        // 保任务目标
b.keep_recent_turns           = 4;        // 保最近上下文
b.max_tool_result_bytes       = 8192;     // 单条 tool_result 上限
b.trim_tool_results_threshold = 90000;    // 90k tokens 触发 retroactive trim
b.on_budget_event             = my_event_cb;
```

运行时改窗口用 `xAgentSessionSetContextWindow(sess, n)`，整组阈值用
`xAgentSessionSetBudget(sess, &b)`（policy 和回调不动）。

### models.json 中的配置（CLI 视角）

CLI 把配置面分成 **顶层** 和 **per-model** 两层，per-model 同字段
覆盖顶层。`policy` 不暴露——CLI 永远跑 `Summarize`。

```json
{
  "default": "kimi",
  "budget": {
    "context_window": 8192,
    "keep_head_turns": 1,
    "keep_recent_turns": 2,
    "trim_tool_results_threshold": 90000,
    "max_tool_result_bytes": 8192
  },
  "models": [
    { "id": "kimi", "provider": "openai", "model": "...",
      "api_key": "...", "base_url": "...",
      "budget": { "context_window": 131072 } }
  ]
}
```

**合并顺序（cascade，per-field）：**

1. xagent 内置默认（C 层硬编码）
2. ↓ 顶层 `budget` 覆盖（仅显式写出的字段）
3. ↓ 当前模型的 `budget` 覆盖（仅显式写出的字段）

切模型时按目标模型重新 cascade 一次，整组阈值通过
`xAgentSessionSetBudget` 一次写回 session；`policy` 和
`on_budget_event` 保持 session 创建时的值。实现位于
`apps/cli/config.cpp::cli_model_config_resolve_budget()`。

---

## 不变量清单

任何策略都必须遵守：

1. **System prompt 不裁** —— borrowed sidecar，裁了也救不了。
2. **当前正在提交的 user turn 不裁** —— 不然用户在问什么就没了。
3. **`tool_use ↔ tool_result` 原子对** —— 切只在 User 边界切。
4. **至少保留 `keep_recent_turns` 个完整 user turn**。
5. **至少保留 `keep_head_turns` 个完整 user turn**（Summarize 专属）。
6. **L1 preserve 喂进去的 = history 里即将消失的**，一一对应。

---

## Prior art / 工程取舍

### 直接借鉴

- **Per-message envelope (`per_msg_envelope`)**：来自 OpenAI cookbook 的
  「Counting tokens for chat completions」（`tokens_per_message=3`、
  `tokens_per_name=1`）。我们合成了一个粗常数，不区分 role / name。
- **Drop-oldest / windowed memory**：`keep_recent_turns` 等价 LangChain
  `ConversationBufferWindowMemory(k=...)` 的 `k`、OpenAI Assistants API
  `truncation_strategy: "auto"`。
- **Summarize 思想**：LangChain `ConversationSummaryBufferMemory`
  是经典参考。我们的实装走 Session 内部 compact Query 的路子。
- **`tool_use ↔ tool_result` 配对**：Anthropic tool use 文档明确要求
  「every `tool_use` block must be followed by a `tool_result`」。
  OpenAI function calling 同约束。
- **Cache-friendly prefix 复用**：Anthropic prompt caching 文档明确说
  cache hit 要求 prefix 字节级一致。我们 compact Query 直接把主对话
  prefix 整段发出去就是冲着 100% 命中去的。

### 本项目自己的工程决定

下面这些没有公开文献对应，是从 moo 的代码形态推出来的：

1. **删校准器 + 增量记账**：用 provider 首轮 `prompt_tokens` 锚精确基线，
   只估增量。校准器收益边际接近零、状态机复杂度反而高，删除净收益。
2. **中段摘要而非 front-replace**：保 head 是为了「记住任务目标」，
   保 tail 是为了「记住当前进度」，摘 middle 是「丢冗余推理」。
   这条切分是从长对话失效模式里反推出来的，不等价于任何现成 memory 库。
3. **Cache-friendly compact Query**：head + middle 整段原样发出去做摘要，
   而不是抽取 / 重组。多花一些 input token，换 prompt cache 100% 命中。
4. **删 Auto / 删 TruncateOldest / 删降级**：把策略空间从「Disabled / Error
   / TruncateOldest / Summarize / Auto」收敛到「Disabled / Error /
   Summarize（推荐） / TruncateTail（兜底）」。少即是多 —— 旧 Auto 阈值
   `tool_ratio = 0.4` 是经验值，且降级 truncate 会掩盖摘要失败的真因。
5. **Splice = `xArrayRemoveRange + xArrayInsert`，不动 head 不动 tail**：
   把「保 prefix」从约定变成数据结构层面的保证。
6. **L1 preserve 是唯一写通道**：所有持久化必须经过 budget event 触发，
   不在其它路径偷偷写 —— 这条保证 L1 数据来源单一、可审计。

### 建议继续阅读

- Anthropic, [Long context tips](https://docs.anthropic.com/en/docs/build-with-claude/prompt-engineering/long-context-tips)
- Anthropic, [Prompt caching](https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching)
- OpenAI, [Managing tokens](https://platform.openai.com/docs/guides/text-generation/managing-tokens)
- Wang et al., [Recursively Summarizing Books with Human Feedback](https://arxiv.org/abs/2109.10862) —— 分块总结的早期工作。

---

## 相关测试

`libs/xagent/session_test.cpp`:

- `BudgetSummarizePreservesHeadAndRecent` —— middle splice 字节级正确性。
- `L1PreserveCompactedDeliversMiddleBandOnly` —— L1 通道不变量。
- `BudgetSummarizeKeepHeadZeroDegradesToFrontReplace` —— `keep_head_turns=0`
  退化兼容。
- `BudgetSummarizeCompactsHistory` —— end-to-end compact pipeline。

未覆盖（已知）：`persisted_prefix` 的「跨 middle」「>= recent_idx」两个分支
还没单独测试 —— 当前测试都从空 L1 store 起步、`persisted_prefix = 0`，
只走「< head_idx」分支。等 prime / resume 路径暴露 bug 再补。
