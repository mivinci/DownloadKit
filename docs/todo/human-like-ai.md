# 让 AI "像人"：xagent 模块的长期产品方向

作者：小W（与麦伯伯讨论后整理）
日期：2026-04-23
状态：draft / 路线图，不是实现规范

---

## 0. TL;DR

- "像人" **不等于** "有记忆"。记忆只是四个维度之一，另外三个是：**情绪延续**、**选择性遗忘**、**主动唤醒**。
- 四个维度的难度递增，SOTA 覆盖度递减。第 1、3 维已有工业方案，第 2、4 维基本空白——**护城河在后两维**。
- 在现有 `xAgent` + `xAgentSession` 架构之上，加三个内部组件即可铺开：`xAgentMemory`（分层记忆）、`xAgentMood`（情绪状态）、`xAgentScheduler`（主动唤醒）。公开 API 几乎不用动。
- 分三期：**MVP（记忆 + 压缩）→ v1（情绪延续）→ v2（主动唤醒）**。每期都给可测指标，不做"感觉更像人"这种玄学验收。
- 明确**不做**什么：不做通用 memory-as-a-service、不做无限上下文幻觉、不做"人格扮演"。

---

## Part I. 问题定义：什么叫"像人"

"像人"是个被滥用的词。先拆清楚。

### 1.1 "有记忆" ≠ "像人"

当下主流的 AI Memory 产品（OpenAI Memory、Letta、MemGPT、A-MEM）都在解决**一个狭义问题**：

> 让 AI 在跨对话时能回忆起用户说过的事实。

这是**必要条件**但**远不充分**。一个有完美事实回忆的 AI 仍然会让人觉得"不像人"——因为它：

- 每次对话都是冷启动情绪（上次聊累了这次还是礼貌八股）
- 啥都记得，包括废话（缺乏**遗忘**这个认知功能）
- 永远 pull-only（你问它才查，从不主动想起来）
- 回忆方式是"检索到 fact 后生硬插入 prompt"，而不是"这段对话让我想起你上次说过……"

真正让人觉得"像人"的，是**这四个维度的组合**：

| 维度 | 一句话 | 工业 SOTA |
| --- | --- | --- |
| 分层记忆 | 区分当下、近期、长期、身份 | ⭐⭐⭐ MemGPT/Letta/A-MEM 在做 |
| 情绪延续 | mood 跨对话 carry-over | ⭐ 基本空白 |
| 选择性遗忘 | 压缩废话，保留高价值节点 | ⭐⭐ 多数是简单 time-decay |
| 主动唤醒 | push 而非 pull，适时提起旧事 | ⭐ 基本空白 |

### 1.2 为什么"像人"值得做

一句话：**这是端侧 agent 相对云端 giant model 的唯一不对称优势。**

- 云端模型（Claude/GPT）在"单次问答能力"上没人追得上，这条路打不过
- 但**持续陪伴**需要：长期一致的记忆、熟悉的情绪基调、低延迟响应、隐私本地化——这**四个点云端都做不好**
- `xagent` 跑在 moo 之上，本身就是轻量 / 嵌入式 / 本地优先的定位，正好吃这条赛道
- 竞品：Character.AI（情绪在线但无持久记忆）、Replika（记忆有但肤浅）、OpenAI Memory（fact only，无 mood）——都没打穿

### 1.3 一个简单的判别准则

一个 AI 是否"像人"，看它在下面这个场景的表现：

> 用户昨天说"最近项目搞崩了，很累"。今天开新会话，用户说："早。"

- **Fact-only AI**：`早上好！今天想做什么？`
- **像人 AI**：`早。昨天说很累，睡得还行吗？`

差距在哪儿？

1. **分层记忆**命中了"昨天聊过什么"（长期）+"刚打招呼"（当前）
2. **情绪延续**记住了"累"这个 mood，没强行 reset
3. **主动唤醒**：用户没问，AI 先提——从 pull 切到 push
4. **选择性遗忘**：没去翻三个月前某句闲聊，只挑相关、近期的

这个测试可以作为 v2 的验收 benchmark。

---

## Part II. 四个维度的深挖

### 2.1 分层记忆（Hierarchical Memory）

#### 现象

人脑的记忆是分层的：

- **工作记忆**（当下对话，7±2 项）
- **情景记忆**（最近几天的具体事件）
- **语义记忆**（长期稳定的事实 / 概念）
- **自传体记忆**（关于"这个人是谁"的连贯叙事）

AI 如果全部塞 context window，两个问题：

1. **容量天花板**——128k 也就聊几天就爆
2. **信号淹没**——废话和重要的事同等权重，模型注意力被稀释

#### 为什么难

- **写入路径**：每轮对话后该存什么、不存什么，这是一个**在线摘要**问题，不是检索问题
- **读取路径**：下一轮该调哪些记忆，这是**语义相关性 + 时间相关性 + 情境相关性**的三维打分
- **一致性**：多轮对话里用户说法矛盾时怎么办（"我喜欢 Python" → 一周后 "我现在主要写 Rust"）

#### SOTA 现状（2025 年底）

**六大方案的详细对比：**

| 方案 | 分层架构 | 写入策略 | 读取策略 | 一致性处理 | 端侧适用 |
| --- | --- | --- | --- | --- | --- |
| **MemGPT / Letta** | 两级：Main Context（system + working + FIFO 消息）/ External Context（Archival 向量库 + Recall 消息历史） | **LLM self-edit**：模型自主调 `core_memory_append/replace`、`archival_insert` 等函数 | 溢出触发 recursive summarization | 靠 LLM 自己覆写 memory block | ❌ 依赖大模型 self-management |
| **A-MEM** | 扁平 + 原子笔记（每条 `{content, timestamp, keywords, tags, context, embedding, links}`） | **LLM 三步**：生成语义属性 → 向量检索 Top-k 邻居 → LLM 决定链接 | 向量 Top-k + 沿链接"同盒子"扩展 | **Memory Evolution**：新记忆会反向改写老邻居的 context/tags | ⚠️ 每次写入 ~1200 tokens LLM call，~$0.0003/次 |
| **Mem0** | User/Session/Agent 三作用域 + Factual/Episodic/Semantic 逻辑分层 + v1.1 图记忆 | **LLM 做 Add/Update/Delete/NOOP 四选一**：新事实与旧记忆冲突时自动 invalidate | 向量检索 + 图关系 | 冲突覆盖（显式） | ⚠️ 写入频繁调 LLM |
| **Memobase** | Profile（长期画像，topic/sub-topic slot）/ Event（时间戳事件流）/ Buffer（短期缓冲） | Buffer 到阈值后 flush 进 Profile，LLM 做 slot merge/rewrite | Profile 全注入 + Event 检索 | Slot 重写 + 长度上限自动浓缩 | ⚠️ Profile slot 设计重 |
| **Memary** | 双层：Knowledge Graph（Neo4j 实体关系）+ Memory Stream（时序）+ Entity Store（按实体聚合 + 频次） | 实体抽取 → KG 入库 | 图推理 + Top-k 过滤 | KG 不删，靠检索阶段软过滤 | ❌ 需要图库基础设施 |
| **ChatGPT Memory** | **四层全量注入**：Metadata / Recent 40 Conversations / Model Set Context（用户显式）/ User Knowledge Memories（AI 压缩） | 定期批量：把最近几百轮对话压缩成 10 段密集摘要 | **无 RAG，无向量**——每次请求全量塞进 context | 仅靠用户显式覆盖（Model Set Context 优先级最高） | ❌ 押的是 context 窗口和成本下降（Bitter Lesson） |

**两条路线的分野：**

这六家其实分成两派：

- **工程派**（MemGPT / A-MEM / Mem0 / Memobase / Memary）：相信结构化分层 + 检索是正道。代价是写入路径有 LLM call 开销。
- **暴力派**（ChatGPT Memory）：赌 Sutton 的 *Bitter Lesson*——不做检索脚手架，全量塞，等模型和 context 窗口解决一切。代价是端侧和 API 用户用不起。

**对 xagent 的启示**：

- xagent 跑在**端侧**，context 成本硬约束——**不能走 ChatGPT 的暴力路线**，必须分层 + 检索。
- A-MEM 的 **Memory Evolution**（新记忆反向改写旧记忆）是真创新，解决了"用户前后矛盾怎么办"的一致性问题。值得吸收。
- Mem0 的 **Add/Update/Delete/NOOP 四选一**是比 A-MEM 更轻的一致性方案，端侧可能更适合。
- **共性缺陷**：工程派的六家写入策略都是"LLM 自己决定"，没有明确的价值函数。结果要么存太多（噪声），要么存太少（漏）。这是我们的机会。

#### xagent 落地思路

四层存储：

```text
┌─────────────────────────────────────┐
│ L0: Working Memory                  │  = xAgentSession 内 messages 数组
│   当前对话的 message 流              │    （已经有了，不用改）
├─────────────────────────────────────┤
│ L1: Episodic Buffer                 │  = 新组件 xAgentEpisode
│   最近 N 轮对话的压缩摘要            │    每轮结束时 LLM 抽取
├─────────────────────────────────────┤
│ L2: Semantic Store                  │  = 新组件 xAgentFact
│   稳定事实（偏好、身份、重要决定）    │    vector + keyword 双索引
├─────────────────────────────────────┤
│ L3: Self Model                      │  = 新组件 xAgentPersona
│   "这个用户是谁"的叙事性画像         │    月级别更新
└─────────────────────────────────────┘
```

**写入价值函数**（避免"LLM 自己决定"的黑箱）：

```text
value(event) = α·recency + β·specificity + γ·emotional_intensity + δ·user_reference_count

α=0.2, β=0.3, γ=0.3, δ=0.2  # 初始权重，后续可学习
```

- **specificity**：事件越具体（专有名词、数字、时间）价值越高（"我在 Tencent 工作"> "我有工作"）
- **emotional_intensity**：对应 Part 2.2 的 mood 模块输出
- **user_reference_count**：用户后续是否又提起过（强信号）

超过阈值才升到 L2，否则过一段时间从 L1 蒸发。

**读取路径**：每轮用户输入进来时，并发查 L1（最近对话摘要，时间优先）+ L2（向量检索，语义优先），取 top-k 加进当轮 system prompt。L3 始终在 system prompt 头部。

**一致性处理（借鉴 Mem0，而不是 A-MEM）**：

用户前后矛盾时（"我喜欢 Python" → 一周后 "我现在主要写 Rust"）怎么办？两个选项：

- A-MEM 路线：**Memory Evolution**，新记忆反向改写老邻居的 context/tags。优雅，但每次写入都要 LLM call，**端侧太贵**。
- Mem0 路线：LLM 判断 **Add / Update / Delete / NOOP 四选一**，只在检测到冲突时才改写。

**选 Mem0 路线**，但优化：

```text
每次要写入 L2 fact 时：
  1. 向量检索出语义最近的 3 条老 fact
  2. 如果相似度 < 0.6：直接 Add（无冲突）            ← 90% 的情况在这里结束，零 LLM call
  3. 如果相似度 >= 0.6：才调 LLM 判断 Add/Update/Delete
```

这样**90% 的写入走快速路径**，只有可能冲突的 10% 才付 LLM 成本。比 A-MEM 便宜一个数量级。

**与 ChatGPT Memory 对比**：我们刻意放弃了它的"全量注入"路线，因为端侧玩不起。但吸收了它**分模块边界清晰**这一点：L0/L1/L2/L3 四层职责不重叠，每层有明确的写入源和生命周期。

### 2.2 情绪延续（Emotional Continuity）

#### 现象（情绪延续）

"记住的不只是事实，还有情绪上下文。" 用户上次聊天累了，这次开场看到"累"这个上下文应该**自然承接疲惫基调**，而不是冷启动回到标准礼貌模式。

举个具体对比：

> 用户 [昨天]：忙了一整天，头都炸了
> 用户 [今天]：下班了

- **Fact AI**：`下班快乐！晚上有什么计划？`
- **Mood AI**：`下班了。昨天头还炸着，今天好点没？`

第二个显然更像人。差别在于：昨天的 mood（疲惫）**没有因为新对话开始而被清零**。

#### 为什么难（情绪延续）

- **情绪不是 fact**——它没有好的结构化表示（"疲惫"能存成 tuple 吗？）
- **衰减曲线不线性**——强情绪可以 carry 几天，弱情绪一觉就散
- **多情绪混合**——同时累 + 兴奋 + 焦虑是常态
- **双向**：AI 的 mood 也会影响用户（AI 持续悲观 → 用户也沮丧）

这个维度**没有现成工业方案**。Character.AI 有情绪但不持久；Replika 有持久但模型很小 mood 表达粗糙。

#### xagent 落地思路（情绪延续）

引入 `xAgentMoodState`，一个**小维度向量**而非 one-hot：

```c
XDEF_STRUCT(xAgentMoodState) {
  float valence;      /* -1 (消极) .. +1 (积极) */
  float arousal;      /* 0 (平静) .. 1 (激动) */
  float fatigue;      /* 0 (精力充沛) .. 1 (疲惫) */
  float confidence;   /* 0 (焦虑/不确定) .. 1 (笃定) */
  uint64_t updated_ms;
};
```

这是 **VAD 模型（Valence-Arousal-Dominance）的工程简化**，心理学有共识基础，不是我拍脑袋。

**更新**：每轮对话结束时，由一个小 classifier（可以是另一个小模型 call，也可以是规则 + 关键词）给 user mood 打分，指数衰减合并到 `xAgentMoodState`。

```text
mood_new = λ·mood_observed + (1-λ)·mood_prev·decay(Δt)
λ=0.3, decay(Δt) = exp(-Δt / half_life)
half_life = 12 小时（可配置）
```

**消费**：mood 序列化进 system prompt，作为"当前用户情绪基线"。模型的回复语气自然被引导。

**注意**：mood 不覆盖回复内容，只影响风格。AI 永远不应该说"我看你很疲惫哦"这种直接暴露检测——要**隐式共情**，像真实熟人。

### 2.3 选择性遗忘（Selective Forgetting）

#### 现象（选择性遗忘）

人会忘。而且忘得**有选择**——忘掉细节，记住感觉；忘掉"吃了什么"，记住"那天很开心"。

AI 如果啥都记，有两个问题：

1. **存储爆炸**
2. **检索污染**——关键信号被海量废话稀释

#### 为什么难（选择性遗忘）

- "什么是废话"没有客观定义
- 压缩（丢信息）是不可逆的，必须谨慎
- 过度压缩 → AI 显得"健忘不靠谱"；压缩不足 → 性能崩溃

#### SOTA 现状（选择性遗忘）（2025 年底）

这一维的**业界方案比 2.1 维分裂得多**——基本没有共识，每家用自己的土办法：

| 方案 | 遗忘策略 | 机制本质 | 问题 |
| --- | --- | --- | --- |
| **Claude Code / Cline compact** | 对话长度到阈值时整段压缩成摘要 | Lossy summarization | 粗暴一刀切，不分重要性 |
| **MemGPT / Letta** | Recursive summarization：旧消息递归总结归档 | 只压缩，不删 | 摘要会越来越长，二次信息失真 |
| **MemoryBank** | **艾宾浩斯遗忘曲线**：每条记忆有 strength，随时间衰减，被访问时增强 | Time + access decay | 接近人类机制，但没看重要性 |
| **Mem0** | LLM 判断 Add/Update/Delete/NOOP + TTL 衰减 | 冲突覆盖 + 时间过期 | 依赖 LLM 每次判断，成本高 |
| **Memobase** | Profile slot 达上限时 LLM 重写浓缩 | 容量驱动的 slot-level 压缩 | 只在容量满时触发 |
| **Memary** | recency + frequency 加权，检索阶段软过滤 | 低频老记忆自然沉底，不真删 | 软遗忘不节省存储 |
| **A-MEM** | **不做遗忘**——用 "Memory Evolution" 代替（老记忆被改写不被删） | 演化替代遗忘 | 存储无限增长；"演化"本身靠 LLM，成本累积 |
| **ChatGPT Memory** | **没有遗忘机制**——摘要一旦生成永久存在 | (none) | 作者自爆：2025 年 10 月的日本旅行计划还在记忆里，实际从未成行 |

**业界共性失败**：

1. **只看时间（recency），不看价值（value）**——LRU 对对话数据是错的前提
2. **压缩=丢信息不可逆**——一旦摘要就找不回细节
3. **LLM 判断成本高**——A-MEM/Mem0 路线每次写入都要调模型，端侧玩不起
4. **没有"情绪峰值保留"**——重要的是情绪强度，不是语义密度

**对 xagent 的启示**：

- MemoryBank 的**艾宾浩斯曲线**是最接近人脑的，可以借鉴
- A-MEM 的**演化**太贵，但它的"不删只改写"哲学可以用于 L3 Persona
- Mem0 的**冲突驱动覆盖**轻量，可以用于 L2 Fact（我们在 2.1 已经借鉴）
- **没人做"情绪峰值保留"**——这是我们的机会

#### xagent 落地思路（选择性遗忘）

双层压缩机制，参考 Claude Code 但做得更细：

##### Layer A: 实时微压缩（每 N 轮触发一次）

- 把最老的 k 轮原始消息合并成一条摘要（`xAgentMessage` role = System，content = "Earlier: ..."）
- 保留用户/AI 的**关键发言原文**（判据：在 mood 峰值 / 包含专有名词 / 用户后续引用过）
- 其余用摘要替代

##### Layer B: 晚期整合（会话结束后异步跑）

- 把当前会话的完整内容抽成一条 Episode（存 L1）
- Episode 结构：

  ```c
  XDEF_STRUCT(xAgentEpisode) {
    uint64_t started_ms;
    uint64_t ended_ms;
    const char *summary;       /* 3-5 句 */
    const char *highlights;    /* 带情绪峰值的原文片段 */
    xAgentMoodState closing_mood;
    const char **fact_refs;    /* 提升到 L2 的 fact id */
    size_t fact_ref_count;
  };
  ```

- Episode 级别用 value function 决定哪些 fact 升 L2

**遗忘曲线**：Episode 本身也会衰减。超过 30 天且从未被引用过 → 降级为纯 summary，丢掉 highlights。超过 180 天且仍未引用 → 删除。

这个机制等于给 AI 加了一条**艾宾浩斯遗忘曲线**。

### 2.4 主动唤醒（Proactive Recall）

#### 现象（主动唤醒）

老朋友的定义之一：**在合适时机主动提起旧事**。

> 用户说过"下周去见客户很紧张"
> 一周后上线：AI 开口："上次那个客户谈得怎样？"

这是**push**，不是 pull。现在所有 AI 产品（除了少数推送式日程提醒）都是 pull——用户不问就永远沉默。

#### 为什么难（主动唤醒）

技术上：

- **时机判断**需要 background scheduler（现有架构都是 event-driven reactive）
- **合适与否**是个品味问题——push 太勤烦人、太稀形同没有
- **内容选择**：哪件旧事值得提？（和"遗忘"反着用同一个 value function）

产品上：

- **边界极其敏感**——push 过度会让用户觉得 AI "监视我"
- 必须对用户可控（静默模式、只在对话中主动提、不做通知推送）

SOTA：**几乎无**。Replika 有一个定时问候但极其机械。

#### xagent 落地思路（主动唤醒）

加一个后台组件 `xAgentScheduler`，架构上和 `xEventLoop` 的 timer 机制对齐：

```c
/* 声明略——关键思路 */
XCAPI(xErrno) xAgentSchedulerArmProactive(
    xAgentScheduler sch,
    xAgentSession sess,
    const xAgentEpisode *source_episode,
    uint64_t not_before_ms,     /* 最早允许 push 的时间 */
    uint64_t not_after_ms,      /* 超过就作废 */
    float priority);             /* 0-1 */
```

**触发条件（AND 全满足才 push）**：

1. 用户**主动开启**新会话（绝不在静默时打扰）
2. 当前会话还没聊到相关话题
3. `source_episode.closing_mood` 有未解悬念（未完成的事、强情绪）
4. 距上次 push 不少于 X 天（避免轰炸）
5. 当前 mood 允许（用户情绪极差时别戳痛点）

**落地形态**：不是独立推送，而是**在用户开启新会话、AI 第一句回复时**，由 scheduler 往 system prompt 里注入一条 "Consider proactively asking about: ..."。**是否真的开口让模型自己决定**——模型读完上下文觉得不合适就不提，天然有一层过滤。

**关键设计**：scheduler 只"建议"，不"强制"。这样模型自己的分寸感成为最后一道过滤。

---

## Part III. 架构草图

在现有 plan.md 描述的两层对象模型上，**不推翻任何公开 API**，加三个内部组件：

```c
┌──────────────────────────────────────────────┐
│                xAgent                      │
│  (能力模板，长生命周期)                        │
│                                              │
│  provider: xAgentProvider                       │
│  tools:    xAgentTool[]                         │
│                                              │
│  ┌───────────── NEW ──────────────────┐     │
│  │ memory:    xAgentMemory               │     │
│  │ mood:      xAgentMoodTracker          │     │
│  │ scheduler: xAgentScheduler            │     │
│  └────────────────────────────────────┘     │
└──────────────────────────────────────────────┘
         │ create
         ▼
┌──────────────────────────────────────────────┐
│                xAgentSession                    │
│  (一次对话实例)                                │
│                                              │
│  messages:  xAgentMessage[]  ← L0 Working Mem   │
│  callbacks: on_text/done/error/tool          │
│                                              │
│  每轮 input/output 时：                        │
│   ↓ 读：memory.retrieve(user_input) → inject │
│   ↓ 读：mood.current() → inject              │
│   ↓ 读：scheduler.pending() → inject         │
│   ↑ 写：memory.observe(turn)                 │
│   ↑ 写：mood.update(turn)                    │
│   ↑ 写：scheduler.consider(turn)             │
└──────────────────────────────────────────────┘
```

**为什么放 Agent 不放 Session**：

- Memory/Mood/Persona 是**跨会话**的——必须随 Agent 生命周期
- Session 是一次对话，短生命；Memory 要比它活得久
- 多个 Session 并发时共享同一份 Memory（带锁，但多数是读多写少）

**为什么公开 API 不用动**：

- 这三个组件的更新都在 `xAgentSession` 内部完成（每次 input/done）
- 使用方从不直接操作 memory/mood
- 暴露点仅两个可选配置项加到 `xAgentConf`：`memory_backend` 和 `persona_init`

---

## Part IV. 三期路线图

**每期都有可测指标，不做"感觉更像人"。**

### MVP：分层记忆 + 选择性遗忘（6-8 周）

**交付**：

- `xAgentMemory`（L0 复用现有 messages，L1 Episode，L2 Fact）
- Layer A 实时微压缩
- 基础 value function

**指标**：

- 长对话（>100 轮）不崩，上下文命中率 ≥ 70%
- 存储增长：每轮 < 500 bytes 平均
- 用户主动引用过的旧事，回忆准确率 ≥ 85%（人工标注 200 条）

**依赖**：

- 需要一个本地嵌入模型（bge-small / all-MiniLM）做向量检索
- SQLite + sqlite-vec（已成熟，别发明轮子）

### v1：情绪延续（4-6 周）

**交付**：

- `xAgentMoodTracker`
- mood classifier（小模型 call 或规则）
- system prompt 注入

**指标**：

- Mood carry-over benchmark：20 组"前后对话"测试，跨会话 mood 连续性人工评分 ≥ 7/10
- A/B：开 mood vs 不开 mood，用户留存 / 满意度对比
- 无 regression：mood on 不应导致回复质量下降（对照组 blind 评测）

### v2：主动唤醒（6-8 周）

**交付**：

- `xAgentScheduler`
- 集成到 Session 首轮 prompt
- 用户控制（关 / 频率 / 场景白名单）

**指标**：

- Push 准确率：人工标注 50 次 push，"合适"率 ≥ 80%
- 骚扰率：≤ 5%（用户打分"烦"的次数 / 总 push 次数）
- 上面 Part I.3 的"早"测试，盲评通过率 ≥ 60%

---

## Part V. 反共识的取舍

**明确不做**：

1. **无限上下文幻觉**
   不追求 "1M context window" 方向。长 context 是**暴力**不是**智能**。人脑工作记忆也就 7±2，靠的是分层和压缩。

2. **通用 Memory-as-a-Service**
   不做 Letta 那种把 memory 抽成通用服务。memory 必须**深度绑定对话架构和情绪**，拆开就不"像人"了。

3. **人格扮演 / roleplay**
   `xAgentPersona` 是**对用户的画像**，不是给 AI 套皮套。Character.AI 那套路我们不跟。

4. **完全 LLM self-management**
   MemGPT 那套"让大模型决定存啥"在云端大模型上能工作，在端侧小模型上会崩。我们用**明确的 value function + 轻量模型辅助**，工程可控。

5. **push 通知**
   scheduler 只在**用户主动开启会话时**注入建议，不做主动弹窗 / 邮件推送。这是底线，破了就变骚扰产品。

---

## 附录 A：与 moo 现有设计的一致性检查

| moo 惯例 | 本方案是否符合 |
| --- | --- |
| 纯 C99、`XDEF_HANDLE` 不透明句柄 | ✅ `xAgentMemory` / `xAgentMoodTracker` / `xAgentScheduler` 都走 handle |
| 事件循环为一等入参 | ✅ scheduler 用 `xEventLoopTimerAfter`，memory 异步写 |
| 依赖显式传入，不自 new | ✅ memory 用到的 sqlite handle 由调用方传入 |
| 回调中指针仅回调期有效 | ✅ memory.retrieve 返回的 fact 列表遵循同约定 |
| 错误码 `xErrno` | ✅ |
| CMake 目标依赖 xbase/xnet/xhttp | ✅ 新增对 sqlite 的 optional 依赖 |

## 附录 B：术语表

- **L0 / L1 / L2 / L3**：分别对应工作记忆 / 情景缓冲 / 语义存储 / 自我模型
- **VAD**：Valence-Arousal-Dominance，心理学情绪维度模型
- **Episode**：一次完整会话压缩后的结构化记录
- **Fact**：从 Episode 中提升出来的稳定语义片段
- **Persona**：关于用户的叙事性长期画像
- **Push vs Pull**：AI 主动提起 vs 用户问了才答

## 附录 C：参考阅读

### 核心论文

- **MemGPT: Towards LLMs as Operating Systems** (Packer et al., 2023) — arxiv:2310.08560
- **A-Mem: Agentic Memory for LLM Agents** (Xu et al., 2025) — arxiv:2502.12110，NeurIPS 2025 poster
- **MemoryBank**（艾宾浩斯遗忘曲线的 LLM 记忆工程化）
- **Memory in the Age of AI Agents: A Survey**（2025 年底最新综述，新加坡国立/人大/复旦等联合发布）
- **Memory OS of AI Agent** — ACL 2025

### 开源实现

- **Letta** (原 MemGPT 产品化) — github.com/letta-ai/letta
- **A-MEM 生产级实现** — github.com/WujiangXu/A-mem-sys
- **Mem0** — github.com/mem0ai/mem0
- **Memobase** — memobase.io
- **Memary** — Neo4j + 向量的个人助理记忆实现

### 产品逆向分析

- **How ChatGPT Memory Works** — shloked.com/writing/chatgpt-memory-bitter-lesson（关键发现：ChatGPT Memory 不用 RAG，全量注入 + AI 压缩摘要）

### 心理学基础

- VAD 情绪模型：Russell (1980), "A Circumplex Model of Affect"
- Ebbinghaus 遗忘曲线（1885）
- Tulving 情景记忆 / 语义记忆区分（1972）

### 本地工程笔记

- Claude Code compact 机制（本地分析文档 `claude-code-agent-loop-analysis.md`）
- xagent 第一批次 plan.md（API 骨架）

---

## 6. MVP 执行边界（2026-04-24 启动）

> 文档 §0-§5 是**路线图**，回答"做什么 / 为什么做"。本节是**执行边界**，回答"MVP 这一期到底做到哪、用什么做、不做什么"。Session/Query 拆分从此节得到合法性——具体拆分方案见 [`xagent_architecture.md`](xagent_architecture.md) §10。

### 6.1 MVP 为什么拆成 MVP-a / MVP-b 两小段

原 §4 的 MVP 范围（L0+L1+L2 全套 + 基础 value function + 双层压缩）**6-8 周做不完**。主要瓶颈是 L2 需要本地 embedding 模型 + sqlite-vec 集成，光依赖引入和端侧打包体积管控就是独立工程。

所以拆成两段，**MVP-a 跑起来 → 看到跨 session 效果 → 再决定要不要做 MVP-b**：

| 段 | 周期 | 核心交付 | 依赖 |
| --- | --- | --- | --- |
| **MVP-a** | 3-4 周 | L0 复用 + L1 Episode 抽取 + JSONL 持久化 + Agent 层 memory 勾子雏形 | 零新依赖（只加 JSONL 文本 IO） |
| **MVP-b** | 3-4 周 | L2 Fact 向量检索 + SQLite + sqlite-vec + embedding 模型集成 | 依赖评估：sqlite-vec 成熟度、embedding 模型选型（bge-small / all-MiniLM） |

MVP-a 不触 L2，意味着跨 session **只有时间索引 + 文本摘要**，没有向量检索。这够不够"像人"？**够用于验证 Part I.3 的"早"测试的一半**——记得昨天聊过什么（情景记忆命中），但答不上"我三个月前提过的某个同事"这种语义模糊的长期回忆。后半部分等 MVP-b。

### 6.2 四条关键决策（拍板记录）

以下是 §4.MVP 留的悬念的正式拍板，**2026-04-24 敲定，写死在本节**。后续实施过程如遇反例要改，必须在本节留修订记录。

#### 决策 1：MVP-a 只做 L0+L1，不做 L2

- **L0**：复用 `xAgentSession` 现有 `messages` 数组，零改动
- **L1 Episode**：新增 `xAgentEpisode` 结构，在 session 终结时抽取
- **L2 Fact**：推到 MVP-b
- **L3 Persona**：推到 v1 之后（和 mood 一起做，见原 §4 v1）

**理由**：L1 单独可验证（Part I.3 的"早"测试只需 L1 就能跑通一半）；L2 的向量检索依赖是独立风险点，不应该绑在 MVP 交付路径上。

#### 决策 2：L1 存储用 JSONL，不引 SQLite

- **MVP-a 存储**：每个 session 一个 JSONL 文件，每条 `xAgentEpisode` 一行
- **文件布局**：`~/.<app>/xagent/episodes/<agent_id>/<YYYY-MM>/<session_id>.jsonl`
- **检索方式**：按时间窗口 scan（MVP-a 检索只需要"最近 N 天"，不需要语义匹配）
- **MVP-b 切 SQLite + sqlite-vec**：迁移脚本提供，老 JSONL 直接归档不删

**理由**：MVP-a 不做向量检索，就不需要 SQLite。引入 sqlite-vec 是 MVP-b 的事，提前引只会让 CMake 依赖、端侧体积、license 审查都提前到账，没收益。

#### 决策 3：L1 抽取用"规则 + 轻量 LLM call"组合，value function 延后

- **MVP-a 抽取策略**：
  1. **规则先过**：明确"值得记"的条目（包含专有名词、数字、时间、URL 等硬特征）直接入库
  2. **不确定时调 LLM**：一条 prompt ≤ 200 tokens，让模型判断 yes/no + 提取摘要
  3. **value function 完整计算推到 MVP-b**：MVP-a 先记下原始信号（specificity 指标、user_reference_count 计数），不做 α/β/γ/δ 加权计算；等 MVP-b 有线上数据了再调权重
- **LLM 选型**：复用 Agent 配置的 provider，不引入第二个 provider；prompt 模板内置

**理由**：value function 的权重调优**必须有线上数据**才合理，现在拍脑袋 α=0.2/β=0.3 完全没根据。MVP-a 只收集信号、不做决策，是最诚实的做法。

#### 决策 4：Session/Query 拆分与 MVP-a 的绑定时序

**序列（硬依赖，不可并行）**：

```c
Step 1 (xagent_architecture.md §10)  [2026-04-24 起，约 3-5 天]
  └─ session.c 内部 query_*/session_* 分组 + on_provider_done 拆三份
  └─ session_test 9/9 全绿作为 Step 2 开工门槛
        ↓
Step 2 (xagent_architecture.md §10)  [Step 1 过 review 后，约 5-7 天]
  └─ 引入 xAgentQuery 类型 + 落实 §8 预留勾子
  └─ 对外 API 零 break
        ↓
MVP-a                              [Step 2 过 review 后，约 3-4 周]
  └─ xAgentEpisode 结构 + 抽取流水线
  └─ JSONL 存储层
  └─ Agent 层 memory 勾子雏形（不暴露公开 API）
  └─ 端到端测试：两个连续 session，第二个开场能引用第一个的 Episode
        ↓
MVP-b                              [MVP-a benchmark 通过后，约 3-4 周]
  └─ sqlite-vec 依赖引入 + embedding 模型集成
  └─ L2 Fact 向量检索
  └─ value function 权重调优（线上数据驱动）
```

**总预算 5-6 周到 MVP-a 交付，跟原 §4 的 6-8 周接近但可验证节点更多。**

### 6.3 MVP-a 可测指标

**交付验收的硬指标**（比原 §4 的指标更严，因为只做 L0+L1）：

- **跨 session 命中率**：构造 30 组"上一 session 说 X → 下一 session 开场"测试，L1 命中率 ≥ 80%（人工标注）
- **Episode 抽取准确率**：从 L1 能恢复出原 session 核心内容，人工评分 ≥ 7/10
- **规则快速路径占比**：≥ 60% 的 Episode 不需要 LLM call 就能决定入库与否（控制成本）
- **存储增长**：每 session 平均 ≤ 2 KB JSONL（L1 只存摘要不存 highlights，MVP-a 阶段）
- **性能**：session 终结时的 L1 抽取延迟 ≤ 500ms（中位数），不阻塞用户下一 session 开启
- **回归**：开 L1 vs 不开 L1，单次对话的 on_text 延迟差 ≤ 10ms（L1 写入不能拖慢主路径）

Part I.3 的"早"测试**不在 MVP-a 验收里**——那个需要 mood（v1）才能真正通过，MVP-a 只覆盖"事实命中"这一半。

### 6.4 MVP-a 明确不做

钉死边界，避免范围漂移：

1. **不做 mood**：情绪延续是 v1 的事，MVP-a 连 `xAgentMoodState` 结构都不声明
2. **不做 scheduler**：主动唤醒是 v2 的事，MVP-a 的 Agent 层没有后台 timer
3. **不做 Layer B 晚期整合**：只做实时 L1 抽取，没有异步 background 压缩任务
4. **不做 L3 Persona**：Agent 层会预留 `persona` 字段但 MVP-a 不写入
5. **不做 value function 加权**：只收集原始信号，不做 α/β/γ/δ 计算
6. **不做跨 Agent memory 共享**：每个 Agent 的 Episode 文件独立，互不串

### 6.5 修订记录

| 日期 | 修订 | 理由 |
| --- | --- | --- |
| 2026-04-24 | MVP 启动，拆 a/b 两段，钉死四条决策 | 决定扣扳机，动 Session/Query 拆分 |

---

## 结语

"像人"不是玄学，是**四个可拆解维度的工程问题**：分层记忆、情绪延续、选择性遗忘、主动唤醒。每一维都能量化、能测。

SOTA 的现状是：第 1、3 维在卷，第 2、4 维几乎空白。**我们的机会在后两维**，尤其是把四维组合起来——没人做过。

在 xagent 现有架构上，这条路**不需要推翻 API**，只需要加三个内部组件（Memory / Mood / Scheduler），分三期交付。

记**人**，比记**事**更重要。
