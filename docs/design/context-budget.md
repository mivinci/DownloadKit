# 上下文预算：xAiSession 的 prompt-size 守门员

> 一套在 **不改 Provider、不改 Query、不侵入业务代码** 的前提下，给 `xAiSession` 加上 "prompt 太长怎么办" 能力的结构化方案。
>
> 本文面向已经熟悉 xKit 三层会话模型（[Agent / Session / Query](three-layer-conversation-model.md)）的读者，描述 Session 层的预算闸门是怎么拆出来的、每一块负责什么、以及我们在 `examples/ai_session.cpp` 里跑到的真实数字是怎么解释的。

---

## TL;DR

每次 `xAiSessionInput` 调用都会经过一个三步流水线：

```text
  incoming user msg + rolling history
              │
              ▼
   ① estimate  —— bytes/4 + envelope
              │
              ▼
   ② calibrate —— EWMA factor × estimate
              │
              ▼
   ③ policy gate (Disabled | Error | TruncateOldest | SummarizeOldest | Auto | …)
              │
    ┌─────────┼─────────┬──────────────┐
    ▼         ▼         ▼              ▼
  proceed   trim &    compact &     refuse with
            retry     retry         xErrno_PromptTooLong
```

核心拆分：

| 模块 | 文件 | 职责 | 是否有状态 |
| --- | --- | --- | --- |
| 估算器 | `budget.c :: ai_budget_estimate_tokens` | "这段 history 大概多少 token" | 无状态，纯函数 |
| 校准器 | `budget.c :: ai_budget_calibrator_*` | 用 provider 返回的 `prompt_tokens` 持续修正估算器的系统性偏差 | 每 Session 一份 EWMA 状态 |
| 裁剪器 | `budget.c :: ai_budget_earliest_keep` | 在保证 `keep_recent_turns` 的前提下，给出最早允许保留的下标 | 无状态，纯函数 |
| 工具占比 | `budget.c :: ai_budget_tool_ratio` | 计算 history 中 ToolUse/ToolResult 的 token 占比 | 无状态，纯函数 |
| Auto 决策 | `session.c :: session_enforce_budget_` (Auto case) | 用工具占比选择 TruncateOldest 或 SummarizeOldest | 复用 Session 的 history |
| 闸门 | `session.c :: session_enforce_budget_` | 把前三者缝起来，按 `xAiBudgetPolicy` 决定放行 / 裁剪 / 拒绝 | 复用 Session 的 history + calibrator |

整套机制默认 (`xAiBudgetPolicy_Disabled`) 是字节级别的 no-op——老 Session 不需要改一行代码。只有当用户显式把 `sconf.budget.policy` 设为非 Disabled 时，闸门才开始工作。

---

## 为什么要在 Session 层做这件事

这个问题可以放在三层中的任一层：

- **Provider 层** 做最简单：`POST /v1/chat/completions` 返回 400 就重试更短的。但这要求 Session 把历史丢给 Provider 再让它决定，失败了还得退回来——**history ownership 会跨层分裂**。
- **Query 层** 也能做，但 Query 只活一次请求，没有办法对 "这次 trim 掉的东西是以后都不再提交" 做出承诺——**裁完了还得回写 Session，又是一次跨层耦合**。
- **Session 层** 是唯一同时满足三个条件的层：① 拥有 rolling history，② 活得比单次 Query 长，③ 能在 Query 发起之前做决定。

所以闸门放 Session 是**被拓扑逼出来的选择**，不是偏好。

---

## 三件套 I：估算器

**目标**：给定任意 `xAiSessionMsg_` 数组，在不调用远端的前提下，对 "这堆东西序列化后发给 provider 大概多少 token" 给一个合理近似。

公式：

```c
tokens ≈ (Σ payload_bytes) / XAI_BUDGET_BYTES_PER_TOKEN
       + n_entries * XAI_BUDGET_PER_MSG_TOKENS
```

两个常量都在 `budget_private.h`：

- `XAI_BUDGET_BYTES_PER_TOKEN = 4`——英文 "一个 token 大约 4 字节" 的经典启发式。CJK 下会高估（真实 1.5~2 bytes/token），紧凑 JSON 下会略低估。
- `XAI_BUDGET_PER_MSG_TOKENS = 8`——每条消息的角色标记 + JSON 框架 overhead。真实 provider 大多在 3~7 token 之间，给 8 是故意偏保守。

按 entry kind 统计 payload：

| kind | payload |
| --- | --- |
| `Text` / `Thinking` | `text_len` |
| `ToolUse` | `strlen(name) + strlen(args)` |
| `ToolResult` | `tool_result_output_len` |

> **为什么故意粗**：这一层想要的是 "便宜、可复现、永不过设计"。精确 tokenizer 会把 Session 和具体模型绑死（Claude 的 BPE 和 GPT-4o 的不一样），而且每次模型变了都要跟着升级。粗估 + 在线校准（下一节）是更稳的组合。

---

## 三件套 II：校准器

粗估必然有系统性偏差。不同 provider、不同语言、不同内容风格，bytes/4 都会偏一个固定比例。校准器的作用：**拿 provider 真实返回的 `xAiUsage.prompt_tokens` 反过来修正本地估算**。

### 状态

每个 Session 带一个小状态：

```c
typedef struct xAiBudgetCalibrator_ {
  double factor;  /* EWMA-smoothed multiplier, 初始 1.0 */
  size_t samples; /* 已接受的观测数，饱和到 SIZE_MAX */
} xAiBudgetCalibrator;
```

### 更新规则

每次 Query 结束、在 `sess_fwd_on_done` 里：

```c
observed = (double) actual_prompt_tokens / estimated_prompt_tokens;
next = (1 - α) * factor + α * observed;           // α = 0.25
factor = clamp(next, MIN_FACTOR, MAX_FACTOR);     // [0.5, 2.0]
samples++;
```

常量（`budget_private.h`）：

- `XAI_BUDGET_CALIBRATION_ALPHA = 0.25`——一次观测把 factor 往新值方向拉 1/4。**5~10 轮收敛**、单个离群样本不会主导下一次决策。
- `XAI_BUDGET_CALIBRATION_MIN_FACTOR = 0.5`、`MAX_FACTOR = 2.0`——硬夹紧。防止 provider 偶尔返回一个莫名其妙的 usage 块把 factor 打飞。

### Opt-out 规则

两种情况**跳过更新**，避免污染 factor：

1. `usage == NULL` 或 `first_round_prompt_tokens < 0`（provider 没给 / 给的是未知哨兵 -1）。
2. `last_prompt_estimate == 0`（gate 没走——Disabled 模式 / 这次 input 没进闸门）。

**为什么不再需要 `ToolUse` 检测？**

旧实现中 `Query.usage.prompt_tokens` 是跨 round **累加**的（见 `query.c:usage_accumulate`），多轮工具对话时该值会膨胀数倍，无法对应到单次 submit 的 prompt 大小，因此用"产出里有没有 ToolUse"来 opt-out。

新实现改为：`prompt_tokens` 取跨轮 **max**（每轮 provider 报的是完整输入量而非增量，
所以 max 就是总输入量），同时在 `xAiQuery_` 中新增 `first_round_prompt_tokens` 字段，
只记录首轮的 `prompt_tokens`。校准器改用 `first_round_prompt_tokens` 与
`last_prompt_estimate` 配对——gate 只在首轮之前执行，所以首轮的 provider 报告才是
唯一可与 gate 估算归因的数据点。这样一来，多轮工具对话也能产生有效的校准信号，
不再需要 opt-out。

### 为什么把 factor 暴露到 calibrator 而不是直接改估算器

两个原因：

1. 估算器仍然是 **纯函数**。想写测试、想在 fixture 上跑，直接调 `ai_budget_estimate_tokens` 就行，不需要构造 Session。
2. `ai_budget_estimate_tokens_calibrated(msgs, n, factor)` 是一个薄壳 adapter——把 "乘 factor 四舍五入" 的逻辑留在一个地方，方便测试复现。

---

## 三件套 III：裁剪器

**目标**：给定 history 和 `keep_recent_turns`，返回一个下标 `idx`，含义是 `msgs[0..idx)` **可以扔**，`msgs[idx..n)` **必须留**。

裁剪器只做 "允许裁到哪" 这件事，**不做 "要不要裁" 的决策**——那是闸门的事。

### 四条不变量（来自 `xAiBudgetPolicy` 的 doc）

所有策略共同遵守：

1. **System prompt 绝不裁**——它是 borrowed、固定大小的 sidecar，裁它也没法修复问题。闸门里 system_prompt 根本不参与 budget 计算（见 `session_enforce_budget_` 的注释）。
2. **当前正在提交的 user turn 绝不裁**——不然用户在问什么就没了。
3. **`tool_use` / `tool_result` 原子对**——裁只能在 "两对之间" 切，不能切在中间。
4. **至少保留 `keep_recent_turns` 个完整 user turn**——一个 "user turn" 是 1 个 `User` 消息 + 它后续所有 `Assistant` / `Tool` 消息（直到下一个 `User`）。

### 关键洞察：只在 User-role 边界切

`ai_budget_earliest_keep` 的实现只返回两种值：① 0（不能裁），② 某个 `User`-role entry 的下标。

这一条自动蕴含不变量 3——`history_append_user_msg` 只会追加 Text 类 User entry，User entry 前面不会有 "orphaned" tool_result。所以 **只要切在 User 边界**，tool_use / tool_result 对就永远不会被拆散。

### 算法

```text
  U = history 中 User-role 的总数
  if U == 0:                       return 0   // 没锚点
  if keep_recent_turns == 0:       return index of last User entry
  if U <= keep_recent_turns:       return 0   // 不够保留
  k = U - keep_recent_turns
  return index of the k-th User entry (0-indexed)
```

例子：`U=5, keep_recent_turns=2` → 从第 3 个 user turn 开始保留（也就是第 3、4、5 个 user turn + 它们的 assistant/tool chatter），前面 2 个 user turn 连带其 assistant 回复全丢。

---

## 第四件：Auto 决策器

**目标**：给定 history，决定 TruncateOldest 和 SummarizeOldest 哪个更合适——然后复用对应策略的代码路径。

### 核心观察

LLM 对纯文本 history 做摘要的能力很强——一段 2000 token 的闲聊经常能压到 500 token 以下，而且关键信息保留得不错。但对 **结构化工具数据**（JSON arguments、tool_result output），摘要几乎一定会丢东西：

- `tool_use` 的 `arguments` 里有 `id: 12345` 这种字段——摘要把它换成 "查询了某个 ID" → 后续模型拿不到 `12345`，**无法继续推理**。
- `tool_result` 里可能是大段 JSON——摘要把它变成 "返回了成功" → 模型丢失了结构化的返回数据。

所以：**工具占比高时截断，文本占比高时摘要**。

### `ai_budget_tool_ratio`

纯函数，返回 `[0.0, 1.0]`：

```c
double ai_budget_tool_ratio(const xAiSessionMsg_ *msgs, size_t n);
```

计算方式：对每个 entry 按和估算器相同的 per-kind 字节公式加权（不是按条目数量），求 `tool_bytes / total_bytes`。这样一条 2 KiB 的 `tool_result` 会比三条 10 字节的 `Text` entry 更有话语权——和闸门看到的 token 压力一致。

### 阈值

`XAI_BUDGET_AUTO_TOOL_RATIO_THRESHOLD = 0.4`

含义：当 history 中 ≥ 40% 的 token 压力来自工具条目时，选择 TruncateOldest；否则选择 SummarizeOldest。

0.4 而不是 0.5 的原因：**摘要失败的代价远高于截断失败**。截断只是丢掉旧信息——用户还能继续对话；摘要失败（丢关键 ID / 误解参数语义）则会让后续推理产出错误结果，且调用方很难发现。所以我们 **偏向截断**：宁可多截一点纯文本，也不要冒着丢结构化数据的风险去摘要。

### 降级保障

Auto 选了 SummarizeOldest 之后，如果 compact 失败（OOM、provider error、空摘要），`sess_fwd_on_done` 里的降级逻辑会自动退到 TruncateOldest——不需要 Auto 决策器做额外处理。

---

## 闸门：把四件套缝起来

`session_enforce_budget_` 在 `xAiSessionInput` 里、**在 history 落盘之前** 跑。位置选在这里有讲究：

- Error 策略可以拒绝而 **不留脏 history**。
- TruncateOldest 策略可以先塑形 history、再让后面的 append 跑在已经合规的底子上。
- Disabled 策略第一行就 `return xErrno_Ok`——**零可测量开销**，老 Session 的行为和以前完全一致。

流程伪代码：

```c
xErrno session_enforce_budget_(s, msg) {
  if (policy == Disabled) return Ok;

  limit    = budget.max_tokens ?: DEFAULT_MAX_TOKENS;   // 128000
  incoming = estimate_incoming_user_tokens_(msg);
  current  = estimate_tokens_calibrated(history, factor);

  if (current + incoming <= limit) {
    last_prompt_estimate = current + incoming;          // 记住给校准器用
    return Ok;
  }

  switch (policy) {
  case TruncateOldest:
    keep = ai_budget_earliest_keep(history, n, keep_recent_turns);
    if (keep > 0) {
      trim_history_front_(s, keep);
      current = estimate_tokens_calibrated(history, factor);
      if (current + incoming <= limit) {
        last_prompt_estimate = current + incoming;
        return Ok;
      }
    }
    return PromptTooLong;   // 裁到极限还不行 → 拒绝

  case SummarizeOldest:
    keep = ai_budget_earliest_keep(history, n, keep_recent_turns);
    // compact Query 压缩 msgs[0..keep)，成功后 re-enter gate
    // compact 失败 → 自动降级 TruncateOldest
    return Busy;            // 异步 compact 已发起，当前 input 暂挂

  case Auto:
    ratio = ai_budget_tool_ratio(history, n);
    if (ratio >= THRESHOLD)       // 工具占比高 → 截断更安全
      → same as TruncateOldest
    else                           // 文本占比高 → 摘要更划算
      → same as SummarizeOldest

  case Error:
    return PromptTooLong;

  case Callback:            // c4+ 之前当作 Error
  default:
    return PromptTooLong;
  }
}
```

### 为什么 system_prompt 不进预算计算

它是 borrowed 的、fixed-size 的，裁剪器压根碰不到它（不变量 1）。如果把它计进预算，一个过大的 system_prompt 会被当成 "常态 history 压力"，让闸门无限尝试裁 history 却永远不够——不如把这道数学做诚实：**"我能不能裁到够"** 这个问题只和可裁的部分有关。

### `last_prompt_estimate` 的生命周期

- 闸门通过时写入 **`current + incoming`**（也就是 provider 实际会算的那份总量）。
- `sess_fwd_on_done` 读取、喂给校准器、**立刻清零**。

清零很重要：万一未来有代码路径跳过了闸门但还是走到了 `on_done`（比如 Disabled 模式下某个实验性的 retry），陈旧的 `last_prompt_estimate` 绝对不能漏进校准器。

---

## 策略矩阵

| 策略 | 当前状态 | 行为 |
| --- | --- | --- |
| `Disabled` | ✅ 实装 | 默认值，闸门整段 short-circuit |
| `Error` | ✅ 实装 | 超过 `max_tokens` 就返回 `xErrno_PromptTooLong`，不改 history |
| `TruncateOldest` | ✅ 实装 | 按 User 边界前向裁剪，裁到 `keep_recent_turns` 还不够就拒绝 |
| `Callback` | ⏳ 预留 | enum 已就位、行为暂同 Error；c4+ 接入调用方自定义 compaction |
| `SummarizeOldest` | ✅ 实装 | 发起 compact Query 压缩旧 history；compact 失败自动降级 TruncateOldest |
| `Auto` | ✅ 实装 | 按工具占比动态选 TruncateOldest 或 SummarizeOldest |

> **为什么预留枚举要退化到 Error 而不是 Disabled**：调用方明确说 "我要 Callback"，然后我们默默给他 Disabled——这会掩盖 bug 好几年。退化到 Error 会让 "你想要的我还没做" 立刻被看见。

---

## 完整信息流（一次 Input 的命运）

```text
  xAiSessionInput(sess, msg)
         │
         ▼
  ┌──────────────────────────────────────────┐
  │ 1. single-flight 检查（s->query != NULL?）│
  └──────────────────────────────────────────┘
         │ ok
         ▼
  ┌──────────────────────────────────────────┐
  │ 2. session_enforce_budget_               │
  │    ├─ estimate (calibrated)              │
  │    ├─ fit? → last_prompt_estimate 记录   │
  │    └─ miss → 按 policy 分派              │
  │              ├─ Truncate → 裁 → 再估     │
  │              ├─ Summarize → compact      │
  │              │   └─ fail → 降级 Truncate │
  │              ├─ Auto → tool_ratio 分流   │
  │              │   ├─ ratio≥0.4 → Truncate │
  │              │   └─ ratio<0.4 → Summarize│
  │              └─ fallthrough → 拒绝       │
  └──────────────────────────────────────────┘
         │ ok
         ▼
  ┌──────────────────────────────────────────┐
  │ 3. history_append_user_msg（commit）     │
  └──────────────────────────────────────────┘
         │ ok
         ▼
  ┌──────────────────────────────────────────┐
  │ 4. build view + xAiQueryCreate/Run       │
  └──────────────────────────────────────────┘
         │
         ▼ (async provider round-trip)
         │
  ┌──────────────────────────────────────────┐
  │ 5. sess_fwd_on_done                      │
  │    ├─ take produced list                 │
  │    ├─ calibrator_update(est,             │
  │    │     first_round_prompt_tokens)      │
  │    ├─ last_prompt_estimate = 0           │
  │    └─ merge produced into history        │
  └──────────────────────────────────────────┘
```

---

## 在 `examples/ai_session.cpp` 里看活的

demo 把闸门配成：

```cpp
sconf.budget.policy            = xAiBudgetPolicy_Auto;
sconf.budget.max_tokens        = 8192;          // 故意留余量
sconf.budget.keep_recent_turns = 2;             // 至少保留最近两轮
```

每次 `on_done` 的最后一行会打印 calibrator 快照：

```text
[done] reason=completed reply_bytes=638 tokens=793/1925 total=2718 budget=1.103x samples=1 est=0
```

| 字段 | 含义 |
| --- | --- |
| `reason` | `xAiDoneReason` 名字 |
| `reply_bytes` | 本轮 assistant 向 `on_text` 吐出的字节数（累计） |
| `tokens=P/C total=T` | `xAiUsage` 里的 `prompt / completion / total_tokens`（跨 round 累加） |
| `budget=<factor>x` | 校准器当前的 EWMA factor，1.0 是出厂值 |
| `samples=<n>` | 校准器已接受的观测数 |
| `est=<n>` | **本轮** gate 记下的 `last_prompt_estimate`（打印前尚未被 `on_done` 清零？—— 已清零，所以稳定显示 0） |

> `est=0` 是 **特性**：打印发生在 `sess_fwd_on_done` 走完 calibrator 更新、**已经把 `last_prompt_estimate` 清零之后**。如果你想看清零前的值，得往 `ai_budget_calibrator_update` 前面挪打印点。

### 观察 A：校准器在真实收敛

在一次跑多轮对话的样本里：

```text
[done] ... budget=1.103x samples=1 ...
[done] ... budget=0.918x samples=6 ...
[done] ... budget=1.125x samples=5 ...
```

真实 factor 稳稳待在 `[0.9, 1.2]` 区间——和 `budget_private.h` 里估算的 "英文/中文混合工作负载的真实比值在 0.7~1.3" 完全吻合。没有跑飞到 clamp 边界，说明 α=0.25 的保守步长没让单个样本主导 factor。

### 观察 B：`keep_recent_turns` 是真正的 "生死阈值"

用户把 `keep_recent_turns` 从 `2` 降到 `1` 以后，**同一个** `max_tokens=2048` 的配置能继续跑下去。原因直接落在 `ai_budget_earliest_keep` 的算法里：

- `keep=2` 时 "最近一轮 user + 上一轮 assistant" **永远不能被裁**。如果恰好上一轮 assistant 吐了一段长文，光这两条就可能 > 2048 → 裁剪器返回 0 → 闸门 fallthrough 到 `PromptTooLong`，**再怎么裁也救不回来**。
- `keep=1` 时只保留**当前 user 消息**。所有历史（包括那段长 assistant 回复）都可以丢。只要 *单条 user 消息本身* 没超 2048，闸门就总能裁到合规。

这是设计的本意：`keep_recent_turns` 实际上决定了 **"裁到最狠能留多少"**，而不只是 "最少要留多少"。二者看起来同义，但对 gate 的终止行为影响完全不同。

### 观察 C：多轮工具对话也能产生校准信号

demo 的工具（`get_time` / `calculator` / `random_int` / `wordcount`）都会触发多 round。旧实现中，带工具调用的 run 会因为 `single_round` 检查而跳过校准——factor 永远停在 1.0。

新实现改用 `first_round_prompt_tokens` 校准：gate 只在首轮之前执行，首轮的 provider 报告是唯一可与 gate 估算归因的数据点。因此：

- 纯文字问答（"写首诗" / "解释一下 XXX"）→ `samples` **会** 增加。
- 带工具调用（"现在几点" → 触发 `get_time`）→ `samples` **也会** 增加。

factor 不再会因为多轮工具对话而漂到 `MAX_FACTOR=2.0` 并卡死，因为我们用的是首轮数据而非累积膨胀的总量。

### 观察 D：`PromptTooLong` 的两条路径

REPL 里撞到这个错会看到两种前缀：

```text
[error] errno=19 msg=...                    ← 异步路径：走过 gate 但 provider 回 400
        hit budget cap — raise ...
```

或

```text
[error] input rejected (errno=19)           ← 同步路径：gate 本地就拒了
        hit budget cap — raise ...
```

同一个 errno，两种触发点：

- **同步路径**：`xAiSessionInput` 返回 `xErrno_PromptTooLong`——gate 本地判死。`on_error` 永远不会 fire，所以 demo 在 `xAiSessionInput` 返回值处再打一遍同样的 hint。
- **异步路径**：gate 放行了（calibrator 偶尔偏乐观），但 provider 真的嫌太长——`on_error` fire。

两条路径都给用户 **同一句建议**：raise `max_tokens` 或 lower `keep_recent_turns`。

---

## 设计检查清单

把这套东西搬到别的系统时，下面几个问题值得一个个回答一遍：

1. **裁剪边界够不够自洽**？不变量 3（tool_use/tool_result 不可拆）对于所有能放进 history 的 entry kind 都成立吗？xKit 里成立，因为 User 消息只承载 Text；别的系统里 User 如果也能带 ToolResult，这条就需要重新论证。
2. **校准器的 `first_round_prompt_tokens` 归因是否足够鲁棒**？当前实现只取首轮的
   `prompt_tokens`，因为 gate 只在首轮之前执行——这是唯一可与 gate 估算干净归因的
   数据点。如果哪天 gate 也需要在后续轮之前执行（比如 background tool 概念），
   则需要为每轮分别保存 estimate/actual 对。
3. **`keep_recent_turns` 会不会和 `max_tokens` 天然冲突**？会。极端情况下
   `keep_recent_turns=10` + `max_tokens=1024` 永远不合规。我们选择 **拒绝**
   而不是静默违反 floor——因为 "用户明确要求保留最近 10 轮" 的承诺比
   "尽量让它跑" 更强。
4. **Disabled 策略的开销到底是多少**？一条 `if` + 一次返回。对于所有 zero-init
   `xAiSessionConf` 的调用方，行为与实现 c2 之前完全 byte-identical。
   这是上线这套机制的硬前提。
5. **Auto 的 tool_ratio 阈值是否对目标工作负载合理**？0.4 是在 "工具调用密集" 场景下
   推出来的（典型：AI agent 反复调 API）。如果目标工作负载是 "长文写作 + 偶尔查字典"，
   工具占比天然 < 10%，Auto 会稳定选 SummarizeOldest——此时可以直接用
   SummarizeOldest 省掉 ratio 计算开销。反之，纯 API 编排场景工具占比常年 > 80%，
   Auto 退化为 TruncateOldest——直接用 TruncateOldest 更省。
   **Auto 的价值在于混合场景**。
6. **SummarizeOldest 的 compact Query 自身会不会再触发预算闸门**？不会——compact 走的是
   `xAiSessionCompact` 内部的一次性 Query，不经过 `xAiSessionInput`，因此不进闸门。
   但 compact Query 的输出（摘要条目）会替换旧 history，如果摘要太长导致仍然超限，
   `sess_fwd_on_done` 的降级逻辑会转到 TruncateOldest。

---

## 相关代码

- 公共 API：`modules/xai/session.h`（`xAiBudgetPolicy`、`xAiBudgetConf`、`xAiSessionConf::budget`）
- 策略闸门：`modules/xai/session.c :: session_enforce_budget_`
- 三件套：`modules/xai/budget.c` + `modules/xai/budget_private.h`
- 测试：`modules/xai/budget_test.cpp`、`modules/xai/session_test.cpp :: BudgetCalibrator / BudgetEnforcement`
- 活体 demo：`examples/ai_session.cpp`

---

## 参考与原创性声明

这套机制**整体是业界成熟做法的组装**，不是新算法。下面把每一块的来路摊开，便于后来者对照着换组件。

### 直接借鉴的通用做法

- **"~4 bytes per token" 估算**：出自 OpenAI 官方
  [Tokenizer 说明](https://platform.openai.com/tokenizer)
  （"a helpful rule of thumb is that one token generally corresponds to
  ~4 characters of text for common English text"）。我们用的是同一条启发式。
- **`per-message overhead` 常量**：公式形态直接参考 OpenAI Cookbook 的
  [`num_tokens_from_messages`](https://github.com/openai/openai-cookbook/blob/main/examples/How_to_count_tokens_with_tiktoken.ipynb)
  （`tokens_per_message=3`、`tokens_per_name=1` 那一段）。我们合成了一个粗粒度常量 `8`，
  没有拆 role / name。- **EWMA / 指数平滑**：Holt 1957、Brown 1956 的经典统计方法。把 EWMA 用作
  "在线修正粗估" 的工程模式在 TCP RTT 估算（RFC 6298 §2、Jacobson 1988）里完全
  同形——我们只是把 "RTT observed / RTT estimated" 换成了
  "prompt_tokens actual / prompt_tokens estimated"。
- **Drop-oldest / windowed memory**：
  - LangChain 的
    [`ConversationBufferWindowMemory(k=...)`](https://python.langchain.com/api_reference/langchain/memory/langchain.memory.buffer_window.ConversationBufferWindowMemory.html)
    ——我们的 `keep_recent_turns` 就是它的 `k`。
  - LlamaIndex 的 [`ChatMemoryBuffer`](https://docs.llamaindex.ai/en/stable/api_reference/memory/chat_memory_buffer/)。
  - OpenAI Assistants API 的 [`truncation_strategy: "auto"`](https://platform.openai.com/docs/api-reference/runs/createRun)——同一思想的官方实现。
- **`SummarizeOldest` 策略**：LangChain 的
  [`ConversationSummaryBufferMemory`](https://python.langchain.com/api_reference/langchain/memory/langchain.memory.summary_buffer.ConversationSummaryBufferMemory.html)
  是成熟参考。我们的实装是在 Session 内部发一次 compact Query，让模型把旧 history
  压缩成一条 Text 摘要条目。
- **"tool_use / tool_result 必须成对"**：Anthropic 在
  [tool use 文档](https://docs.anthropic.com/en/docs/agents-and-tools/tool-use/overview)
  里明确过——"every `tool_use` block must be followed by a `tool_result`"。
  OpenAI function calling 也有对应约束。

### 本项目自己做的工程取舍（不是 novelty，是局部决定）

下面这些没有对应的公开文献，是从 xKit 的具体代码形态推出来的：

1. **校准器改用 `first_round_prompt_tokens` 归因**（见
   [三件套 II / Opt-out 规则](#三件套-ii校准器)）。旧实现因
   `query.c :: usage_accumulate` 的跨 round 累加语义而需要 `single_round`
   opt-out；新实现改为 `prompt_tokens` 取 max + 首轮归因，多轮工具对话也能产生
   有效校准信号。大多数 memory / truncation 库不做在线 estimator 校准，所以也
   不需要处理这个 corner case。换一套 provider / Query 模型，归因逻辑要重新推。
2. **三件套的职责切分**：estimator 纯函数、calibrator 有状态、trimmer 只回答 "能裁到哪"、policy gate 决定 "要不要裁 / 拒还是通过"。这条拆法是我们自己的，不等价于任何现成库的架构。
3. **Gate 位置 = history 落盘之前**，换来 "Error 策略不留脏 history" 这条
   对调用方的承诺。
4. **裁剪器只在 User 边界切**，用这一条几何约束把 "不拆 tool_use/tool_result 对" 从一条运行时检查变成结构性保证。思想来自 Anthropic 的原子对要求，实现路径是我们自己的。
5. **Auto 策略的 tool_ratio 阈值取 0.4 而非 0.5**——摘要失败的代价（丢关键 ID / 误解参数语义 → 静默错误推理）远高于截断失败（丢旧信息 → 用户还能继续对话），所以偏向截断是理性选择。阈值不是从任何论文推出来的，是我们对 LLM 摘要结构化数据能力的经验判断。

### 建议继续阅读

- Anthropic, ["Long context tips"](https://docs.anthropic.com/en/docs/build-with-claude/prompt-engineering/long-context-tips) —— context 管理的 official guidance。
- OpenAI, ["Managing tokens"](https://platform.openai.com/docs/guides/text-generation/managing-tokens)。
- 如果打算接 `SummarizeOldest`：Wang et al., ["Recursively Summarizing Books with Human Feedback"](https://arxiv.org/abs/2109.10862) —— 分块总结的早期工作。
