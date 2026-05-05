# 分层记忆体系：L1–L4 的职责、协议与落地路径

> 一套在 **不推翻现有三层架构（Agent / Session / Query）**、**不破坏公开 API** 的前提下，给 `xAgent` 加上"全量记忆 → 长期存储 → 情绪追踪 → 自我认知与主动行为"四层能力的结构化方案。
>
> 本文面向已经熟悉 moo 三层会话模型（[Agent / Session / Query](three-layer-conversation-model.md)）、[上下文预算](context-budget.md)和 [类人 AI 四维度](../todo/human-like-ai.md)的读者，描述 L1–L4 每一层住在哪里、跟谁交互、数据怎么流动，以及每层的落地次序。

---

## TL;DR

四层从底到顶，每一层只依赖下面的层，不反向穿透：

```text
┌───────────────────────────────────────────────────────┐
│ L4  自我认知与主动行为                                   │
│   Agent 知道"我是谁"、能力边界、该不该主动开口           │
│   依赖：L2 记忆 + L3 状态量 + 自身人格设定              │
├───────────────────────────────────────────────────────┤
│ L3  情绪 / 状态追踪                                      │
│   追踪 mood delta、活跃度、疲劳度                         │
│   依赖：L1 的产出信号                                    │
├─────────────────────────────────────────────────────────┤
│ L2  长期记忆存储与检索                                    │
│   持久化 L1 提取物；创建 Session 时注入相关记忆            │
│   依赖：L1 的提取结果                                    │
├─────────────────────────────────────────────────────────┤
│ L1  Session 即时记忆（全量）                              │
│   保存 Session 内全部消息（user + assistant + tool）       │
│   依赖：Session / Query 的回调钩子                       │
└─────────────────────────────────────────────────────────┘
```

| 层 | 住哪 | 有状态 | 核心动作 | 落地状态 |
| --- | --- | --- | --- | --- |
| **L1** | `xAgentSession` 的 `on_l1_preserve` 钩子 + `agent.c` 的 JSONL 持久化 | 无（全量消息即交出） | 保存 Session 全量消息；session 销毁前交付给 L2 | ✅ `on_l1_preserve` 回调 + agent 端 JSONL 持久化已落地 |
| **L2** | `xAgent` 内部 | 有（持久化存储） | 存储 L1 产物；`xAgentCreateSession` 时注入 | ❌ 未开始 |
| **L3** | `xAgent` 内部 | 有（running state） | 追踪 mood / 活跃度 / 疲劳度 | ❌ 未开始 |
| **L4** | `xAgent` 内部 | 有（人格 + 调度器） | 人格渲染 + 主动创建 `origin=SystemSynthesized` Session | ❌ 未开始 |

---

## 为什么要把记忆拆成四层

拆层的理由跟三层会话模型拆 Agent / Session / Query 一样：**生存期不同，归属不同，消费者不同**。

如果把"记忆"塞进一个 `xAgentMemory` 对象：

1. **写入路径冲突**——Session 结束时的沉淀（L1→L2）和 Agent 后台反刍（L4）都要写同一个 store，锁冲突。
2. **读取路径分裂**——Query 需要 L0 工作记忆，Session 需要 L1 情景缓冲，Agent 需要 L2/L3 长期状态。一个 store 要服务三个完全不同的查询模式，要么接口膨胀要么性能差。
3. **生命周期纠缠**——L0 随 Session 生死，L2 跨 Session 持久，L3 比 L2 还稳。混在一起，"什么时候该删"变成一笔糊涂账。

拆成四层后，每一层的生存期、写入者、消费者都是明确的——和三层会话模型的切法一脉相承。

---

## L1：Session 即时记忆（全量）

### 职责 — 全量对话记录

L1 是 Session 的**即时记忆**，保存该 Session 内发生的**全部消息**——System、User、Assistant、Tool，不遗漏任何角色和类型。

L1 的语义是 **"这次会话发生了什么"**——完整的对话记录，是后续所有提取、汇总、情绪推断的原始素材。

### 核心原则：L1 是全量，不是摘要

L1 保存的是 Session 的**全量消息流**——user + assistant + tool 全角色全类型，而非仅从 assistant 产出中提取的摘要。

**为什么必须是全量？**

1. **因果链完整**：用户说"我喜欢 Python"，assistant 回复"好的"。如果 L1 只有 assistant text，就只看到"好的"——根本无法提取出用户偏好。没有 user 消息，L2 提取就缺了关键的因果链。
2. **上下文不丢失**：tool 调用的参数和结果、assistant 的思考过程，都是后续提取不可缺失的素材。
3. **L2 提取的输入质量**：L1 全量 → L2 能理解因果、意图、偏好；L1 只有 assistant → L2 只能做浅层关键词匹配。

### L1 与 L2 的关系

```text
┌──────────────────────────────────────────────────┐
│  L1 - Session 即时记忆 (全量)                      │
│                                                    │
│  来源: session.history_arr (全角色全类型)            │
│  内容: System + User + Assistant + Tool 全量消息    │
│  语义: "这次会话发生了什么" — 完整对话记录            │
│  存储: memory.jsonl (on_l1_preserve 回调)           │
│  生命周期: 跟随 Session，Session 销毁时 L1 消失      │
├──────────────────────────────────────────────────┤
│  L2 - Agent 抽象记忆 (提取后)                       │
│                                                    │
│  来源: 从 L1 全量消息中提取                          │
│  内容: Preference / Fact / Decision / Summary       │
│  语义: "Agent 从会话中学到了什么"                    │
│  存储: observations.jsonl                           │
│  生命周期: 跨 Session 持久化                         │
└──────────────────────────────────────────────────┘
```

| 维度 | L1（Session 即时记忆） | L2（Agent 抽象记忆） |
| --- | --- | --- |
| **范围** | 全量消息 (user + assistant + tool) | 结构化观察 (preference / fact / decision) |
| **存储形式** | 原始文本 | 结构化摘要 |
| **归属** | Session | Agent |
| **生命周期** | 随 Session 生死 | 跨 Session 持久 |
| **消费者** | L2 提取器 | 新 Session 注入、L4 唤醒决策 |

### 为什么"L1 全量在 Session，L2 提取在 Agent"

这是从 [xagent_architecture.md](../todo/xagent_architecture.md) §2.3 继承的硬要求。简述：

- **多 Session 并存时的写冲突**：Session A 和 Session B 同时上报"用户偏好 tab"，Agent 层能去重；Session 层各写各的就会留两条。
- **全局视野缺失**：某条事实在单个 Session 里价值一般，但**跨 Session 反复出现 5 次**才显出它是稳定事实。去重计数必须由"看得到全局"的层做。
- **L1 是单 Session 维度的完整快照，L2 才有跨 Session 的全局视野**。

### L1 数据写入点

L1 的写入（即 Session 全量消息的采集）通过 **`on_l1_preserve` 回调**实现，已在 Session 层和 Agent 层落地。

核心设计：`xAgentSessionConf` 新增 `on_l1_preserve` 回调 + `l1_preserve_owner`，在以下三个时机触发：

1. **TruncateOldest 裁剪前**（`xAgentL1PreserveReason_Truncated`）：
   传递即将被丢弃的 `entries [0, keep)`，回调在 `session_trim_history_front_`
   之前触发，保证数据仍然有效。
2. **SummarizeOldest compact 替换前**（`xAgentL1PreserveReason_Compacted`）：
   传递即将被 summary 替换的原始 `entries [0, keep_idx)`，Consumer 可以
   保留原始全量条目（虽然 session history 只留 summary）。
3. **Session teardown 时**（`xAgentL1PreserveReason_Finalizing`）：
   在 `on_finalizing` 之前传递全量剩余 history，确保从未触发 budget
   的 session 也能将完整对话交付 L1。

回调签名：

```c
typedef void (*xAgentSessionL1PreserveFunc)(
  xAgentSession sess, const xAgentSessionMsg *msgs, size_t n_msgs,
  xAgentL1PreserveReason reason, void *owner);
```

语义：entries 只在回调期间有效，Consumer 必须 deep-copy 需要保留的内容。
NULL 回调 = 不做 L1 采集（默认行为，向后兼容）。

Agent 层自动注入：`xAgentCreateSession` 在创建 session 时，如果 agent 配置了
`agent_id` 和 `data_dir`，且调用方未自行提供 `on_l1_preserve`，则自动注入
内置的 `agent_l1_preserve_cb_`——将每条消息以 JSONL 格式追加写入
`{data_dir}/agents/{agent_id}/sessions/{session_id}/memory.jsonl`。

### L1 提取逻辑的实现路径

L1 作为全量即时记忆，其提取逻辑（L1 → L2 的转化）在 Agent 层执行：

1. **L1 的产物结构化 schema**（L2 存储格式）：

```c
XDEF_ENUM(xAgentObservationKind) {
  xAgentObservationKind_Preference,   /* "我喜欢简洁的代码风格" */
  xAgentObservationKind_Fact,         /* "项目使用 UTF-8 编码" */
  xAgentObservationKind_Decision,     /* "决定用 SQLite 而不是 JSONL" */
};

XDEF_STRUCT(xAgentObservation) {
  xAgentObservationKind kind;
  const char *content;    /* 结构化摘要，非原文 */
  float       confidence; /* 0..1，规则路径给 1.0，LLM 路径给模型输出 */
  const char *source_id;  /* 产出它的 session id，供 L2 去重 */
};
```

2. **提取策略**（在 Agent 层从 L1 全量消息中提取 L2 观察）。两阶段：
   - **规则先过**：检测硬特征（专有名词、数字、时间、URL、明确偏好词"我喜欢/讨厌/用…"），匹配则直接构造 `xAgentObservation`，confidence=1.0。
   - **不确定时调 LLM**：一次 prompt ≤ 200 tokens，让模型判断 yes/no + 提取摘要。这个 LLM call 复用 Agent 配置的 provider。

3. **`on_finalizing` 内的汇总逻辑**。用一次 LLM call 做会话级摘要，输出若干 `xAgentObservation` + mood delta + 未完结话题。

4. **提取结果交付给 L2**。L2 模块裁决是否落盘（去重、合并、淘汰）。

### 提取的成本控制

MVP-a 阶段的目标：**≥ 60% 的观察不需要 LLM call 就能决定入库与否**。规则快速路径覆盖明确偏好和硬事实；只有模糊陈述才走 LLM。这样 90% 的写入走零成本路径。

---

## L2：长期记忆存储与检索

### 职责 — 持久化与注入

1. **持久化** L1 提取的结构化记忆。
2. 在 `xAgentCreateSession` 创建新 Session 时，**检索与当前上下文相关的记忆，注入到 system prompt**。
3. 管理记忆的**淘汰与合并**。

### 记忆存储后端

两阶段选型：

| 阶段 | 方案 | 优点 | 缺点 |
| --- | --- | --- | --- |
| **MVP-a** | JSONL 文件 | 零依赖、易调试、易手工修 | 只能按时间索引，无语义检索 |
| **MVP-b** | SQLite + sqlite-vec | 语义检索、灵活查询 | 加依赖、加构建体积 |

MVP-a 的文件布局（与 L1 采集的 `agent_l1_preserve_cb_` 输出路径对齐）：

```text
{data_dir}/agents/{agent_id}/sessions/{session_id}/memory.jsonl
```

每条消息一个 JSONL 行，包含 `role`、`kind` 和对应的 payload 字段。
L2 的 `xAgentObservation` 提取结果将写入同一 agent 目录下的
`observations.jsonl`。MVP-b 切 SQLite 时提供迁移脚本，老 JSONL 归档不删。

### 记忆注入到新 Session

`xAgentCreateSession` 的注入协议（继承 [xagent_architecture.md](../todo/xagent_architecture.md) §2.1）：

1. **人格描述 / 风格约束**：注入到 system prompt，跨 Session 一致。
2. **记忆前缀**：Agent 根据 Session 类型/意图挑选相关记忆，打包成结构化上下文塞进 system prompt。**Session 不反向查询 Agent 的记忆仓**——避免 Session 层需要理解记忆索引。
3. **Mood 初始值**（v1 之后）：从 Agent 当前 mood state 拷贝给 Session。

注入的数据流：

```text
xAgentCreateSession(agent, conf)
      │
      ├── 构建 memory_prefix = agent->memory.retrieve(conf->intent_hint)
      ├── 构建 persona_prefix = agent->persona.render()
      ├── 构建 mood_init      = agent->mood.snapshot()
      │
      └── 注入到 Session 的 system_prompt 前缀
          session->system_prompt = persona_prefix + memory_prefix + original_prompt
```

### 记忆淘汰 / 合并策略

长期运行后记忆条目会膨胀。三个机制：

1. **相似合并**：借鉴 Mem0 的 **Add/Update/Delete/NOOP 四选一**——写入前先向量检索语义最近的 3 条老 fact，相似度 < 0.6 直接 Add（零 LLM call），≥ 0.6 才调 LLM 判断 Add/Update/Delete。90% 的写入走快速路径。

2. **时间衰减**：借鉴 MemoryBank 的**艾宾浩斯遗忘曲线**。Episode 超过 30 天未被引用 → 降级为纯 summary（丢 highlights）；超过 180 天 → 删除。

3. **容量上限**：LRU + 优先级排序。当 fact 数量超过配置上限时，优先淘汰低 confidence + 低 reference_count + 长期未被检索到的条目。

---

## L3：情绪 / 状态追踪

### 职责 — 状态追踪

1. 追踪每次会话的**情绪变化量**（mood delta）。
2. 维护 Agent 级的**活跃度 / 疲劳度**模型。
3. 为 L4 的唤醒频率提供**状态信号**。

### Mood delta 追踪

Mood 不用连续浮点（难解释难 debug），用**小维度向量**：

```c
XDEF_STRUCT(xAgentMoodState) {
  float valence;      /* -1 (消极) .. +1 (积极) */
  float arousal;      /* 0 (平静) .. 1 (激动) */
  float fatigue;      /* 0 (精力充沛) .. 1 (疲惫) */
  float confidence;   /* 0 (焦虑) .. 1 (笃定) */
  uint64_t updated_ms;
};
```

这是 VAD 模型（Valence-Arousal-Dominance）的工程简化，心理学有共识基础。

**更新公式**：

```text
mood_new = λ · mood_observed + (1-λ) · mood_prev · decay(Δt)
λ = 0.3
decay(Δt) = exp(-Δt / half_life)
half_life = 12 小时（可配置）
```

**信号来源**：

- `on_finalizing` 中记录 mood delta。不需要额外 LLM 调用，从 usage pattern 推断：
  - 对话长度极短 → 可能敷衍 / 疲惫
  - tool 调用频繁 → 可能焦虑 / 紧迫
  - 连续感谢 / 表扬 → valence 正向
  - 连续追问同一问题 → confusion 高

**消费方式**：mood 序列化进 system prompt，作为"当前用户情绪基线"。模型的回复语气自然被引导。**注意**：mood 不覆盖回复内容，只影响风格——AI 永远不应该说"我看你很疲惫"这种直接暴露检测。

### 活跃度 / 疲劳度模型

追踪 Agent 的"工作状态"：

```c
XDEF_STRUCT(xAgentVitality) {
  int    sessions_last_24h;     /* 近 24h 内完成的 session 数 */
  size_t tokens_last_24h;      /* 近 24h 内消耗的总 token 数 */
  float  activity_score;       /* 0 (空闲) .. 1 (高负载) */
  float  fatigue_score;        /* 0 (精力充沛) .. 1 (过劳) */
};
```

- **activity_score** → 影响 L4 的唤醒频率（活跃时少打扰，空闲时可以提醒）。
- **fatigue_score** → 触发 context budget 更激进的压缩策略（累的时候上下文要更精简）。

两者的信号都上报给 L4，作为唤醒决策的输入。

---

## L4：自我认知与主动行为

### 职责 — 自我认知与主动行为

L4 是 Agent 的**自我认知层**，回答"我是谁"和"我该不该行动"两个问题：

1. **自我认知**：持有并渲染 Agent 的人格设定（persona），决定"我以什么身份说话"、"我擅长什么"、"我的边界在哪里"。
2. **主动行为**：基于自我认知 + L2 记忆 + L3 状态，决策是否主动创建 `origin=SystemSynthesized` 的 Session，调 `xAgentSessionInput` 发起主动对话。

人格设定（persona）之所以放在 L4 而非 L3，是因为：人格是**静态配置 + 记忆沉淀**的组合体，它的消费者是"决定以什么身份主动开口"——这正是 L4 的职责。L3 只负责情绪/状态的动态追踪，不需要理解"我是谁"。

### 三种 Session 的交互模型

| 场景 | Origin | 谁创建 | 谁销毁 | 生命周期 |
| --- | --- | --- | --- | --- |
| Default Session | `User` | Agent 创建时自动 | Agent 销毁时自动 | 跟 Agent 同生共死 |
| 用户新开 Session | `User` | 用户调 `xAgentCreateSession` | 用户调 `xAgentSessionDestroy` | 用户控制 |
| Agent 主动唤醒 Session | `SystemSynthesized` | Agent 内部 | Agent 内部 | Agent 控制 |

Default Session 是用户的默认入口——用户可以随时跟它对话，不用显式创建。Agent 也可以自己唤醒自己主动新开一个 Session 叫用户——这种 Session 的 input 不是用户发的，是 Agent 合成的。

### 唤醒场景

1. **定时提醒**："该复查代码了"
2. **任务完成通知**：后台 tool 执行完毕
3. **主动建议**："我发现一个优化点"
4. **情感关怀**：上次用户说很累，隔天问候

### 唤醒策略

基于状态量决定是否唤醒——**建议而非强制**：

```text
触发条件（AND 全满足才考虑唤醒）：
  1. 用户主动开启新会话（绝不在静默时打扰） ← 默认策略，可配置
  2. 当前会话还没聊到相关话题
  3. L2 命中了未完结 / 强情绪 episode
  4. 距上次唤醒 ≥ X 天
  5. L3 活跃度允许（高负载时少打扰）
```

**关键设计**：scheduler 只往 system prompt 里注入一条 "Consider proactively asking about: ..."，**是否真的开口让模型自己决定**。模型读完上下文觉得不合适就不提——天然有一层过滤。

### 唤醒 Session 的生命周期

```text
1. Agent 调度器（timer / 事件）决定"该起一个新 Session 了"
2. Agent 调 xAgentCreateSession，origin = SystemSynthesized
3. Agent 组装 nudge input："对了，关于 X..."
4. Agent 调 xAgentSessionInput(session, nudge_input)
5. Session 像普通对话一样跑起来
6. 对话结束后 xAgentSessionDestroy
```

**与 Default Session 的关系**：两者独立，互不干扰。Default Session 是用户的默认入口（`origin=User`），唤醒 Session 是 Agent 主动发起（`origin=SystemSynthesized`）。同一时刻可以共存。

### 用户体验底线

1. **默认保守**——宁可错过主动时机也不要乱刷屏。
2. **用户可控**——提供"关闭 / 降频 / 场景白名单"开关。
3. **不在静默时打扰**——scheduler 默认只在用户开启新会话时注入建议，不做主动弹窗 / 推送。

---

## 四层数据流全景

```mermaid
graph TB
  subgraph "Agent 层（持久）"
    L2["🟡 L2 长期记忆<br/>Fact / Episode / Persona"]
    L3["🟡 L3 情绪/状态<br/>Mood / Vitality"]
    L4["🟡 L4 自我认知与主动行为<br/>Persona / Timer / Event"]
  end

  subgraph "Session 层（临时）"
    L1_extract["🔵 L1 提取器<br/>on_finalizing"]
    L0["🔵 L0 工作记忆<br/>messages 数组"]
  end

  U["👤 User"] -->|"input"| L0
  L0 -->|"全量消息"| L1_extract
  L1_extract -->|"xAgentObservation[]"| L2
  L1_extract -->|"mood delta"| L3

  L2 -->|"记忆前缀注入"| L0
  L3 -->|"mood 初始值"| L0
  L4 -->|"persona + 唤醒建议"| L0

  L3 -->|"活跃度/疲劳度 + persona 微调"| L4
  L2 -->|"未完结 episode"| L4

  L4 -->|"SystemSynthesized"| NEW_SESS["🔵 新 Session"]

  style L2 fill:#FFE5B4,stroke:#E8A87C,color:#5D4037,stroke-width:2px
  style L3 fill:#FFE5B4,stroke:#E8A87C,color:#5D4037,stroke-width:2px
  style L4 fill:#FFE5B4,stroke:#E8A87C,color:#5D4037,stroke-width:2px
  style L1_extract fill:#B5D8F0,stroke:#7FB3D5,color:#1B4965,stroke-width:2px
  style L0 fill:#B5D8F0,stroke:#7FB3D5,color:#1B4965,stroke-width:2px
  style U fill:#F3E5F5,stroke:#CE93D8,color:#4A148C,stroke-width:2px
  style NEW_SESS fill:#B5D8F0,stroke:#7FB3D5,color:#1B4965,stroke-width:2px
```

**读路径**（每个 Session 创建时）：

```text
Agent → Session 注入四样东西：
  1. persona_prefix  (来自 L4 的自我认知)
  2. memory_prefix   (来自 L2 的检索结果)
  3. mood_init       (来自 L3 的当前状态)
  + scheduler_hint   (来自 L4 的唤醒建议，如果有)
```

**写路径**（每个 Session 结束时）：

```text
Session → Agent 上报四样东西：
  1. L1 观察候选 → L2 裁决落盘
  2. mood delta → L3 更新 running state
  3. 人格微调信号 → L4 更新 persona
  4. 生命周期事件 → L4 更新调度状态
```

---

## 与三层会话模型的关系

L1–L4 不是"另外一套架构"，是**三层会话模型在记忆维度的自然展开**：

| 三层概念 | 对应的记忆层 | 原因 |
| --- | --- | --- |
| **Query** | 不涉及 | Query 无状态，不碰记忆 |
| **Session** | L0 + L1 | Session 拥有 messages（L0），负责 L1 提取 |
| **Agent** | L2 + L3 + L4(含自我认知) | 跨 Session 持久的状态和自我认知必须挂在 Agent 上 |

这和 [three-layer-conversation-model.md](three-layer-conversation-model.md) 里"Session 结束时有一个沉淀时刻"的判断完全一致——L1 → L2 的裁决就是那个沉淀。

---

## 与现有代码的映射

| 代码位置 | 已有 | 待加 |
| --- | --- | --- |
| `session_private.h` :: `on_finalizing` | ✅ 钩子字段 | L1 汇总逻辑 |
| `session.h` :: `on_l1_preserve` | ✅ 回调字段 + 三时机触发 | — |
| `agent.c` :: `agent_l1_preserve_cb_` | ✅ JSONL 持久化 | — |
| `agent.c` :: `xAgentCreateSession` | ✅ 自动注入 `on_l1_preserve` | L2 记忆注入、L3 mood 注入 |
| `agent.h` :: `xAgentCreateSession` | ✅ 创建 Session | L4 SystemSynthesized Session 创建 |
| `session.h` :: `xAgentInputOrigin` | ✅ `User` / `SystemSynthesized` 枚举 | — |
| `agent.c` / `agent.h` | — | `xAgentMemory` 内部组件 |
| `agent.c` / `agent.h` | — | `xAgentMoodTracker` 内部组件 |
| `agent.c` / `agent.h` | — | `xAgentPersona` 内部组件 |
| `agent.c` / `agent.h` | — | `xAgentScheduler` 内部组件 |

**核心原则**：这些组件的更新都在 `xAgentSession` 内部完成（通过预留的钩子），使用方从不直接操作 memory / mood / scheduler / persona。公开 API 几乎不用动。

---

## 落地次序

```text
  now                                                    future
   │                                                        │
   ├── L1 采集机制 ✅ ──────────────────────────────┐      │
   │   ● on_l1_preserve 回调（三时机触发）           │      │
   │   ● agent_l1_preserve_cb_ JSONL 持久化         │      │
   │   ● xAgentCreateSession 自动注入             │      │
   │                                                 │      │
   ├── L1 → L2 提取逻辑（当前钩子是空 stub）──┐     │      │
   │   ● xAgentObservation schema                   │     │      │
   │   ● on_finalizing 规则 + LLM 提取           │     │      │
   │   ● 提取结果交付接口                         ↓     ↓      │
   │                                                        │
   ├── L2 长期记忆 ─────────────────────────────────┐      │
   │   ● MVP-a: JSONL 存储                          │      │
   │   ● 记忆注入到新 Session                        │      │
   │   ● MVP-b: SQLite + sqlite-vec + 向量检索       │      │
   │   ● 淘汰 / 合并策略                             ↓      │
   │                                                        │
   ├── L3 情绪 / 状态追踪 ──────────────────────────┐      │
   │   ● xAgentMoodState 结构                          │      │
   │   ● Mood delta 追踪                             │      │
   │   ● 活跃度 / 疲劳度模型                         ↓      │
   │                                                        │
   └── L4 自我认知与主动行为 ────────────────────┘      │
       ● Persona 人格渲染与注入                            │
       ● 定时 / 事件驱动唤醒框架                          │
       ● 唤醒策略（基于 L2+L3 状态量）                    │
       ● SystemSynthesized Session 生命周期管理            │
                                                             │
```

每层都有独立的可测指标（见 [human-like-ai.md](../todo/human-like-ai.md) §6），不做"感觉更像人"这种玄学验收。

---

## 参考

- [三层会话模型：Agent / Session / Query](three-layer-conversation-model.md)
- [上下文预算：Session 的 prompt-size 守门员](context-budget.md)
- [类人 AI 的四个维度](../todo/human-like-ai.md)
- [xagent 三层架构设计方案](../todo/xagent_architecture.md)
- MemGPT: Towards LLMs as Operating Systems (Packer et al., 2023)
- A-MEM: Agentic Memory for LLM Agents (Xu et al., 2025)
- Mem0 — github.com/mem0ai/mem0
- MemoryBank — 艾宾浩斯遗忘曲线的 LLM 记忆工程化
- Russell (1980), "A Circumplex Model of Affect" (VAD 情绪模型)
