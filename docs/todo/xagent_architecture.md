# xagent — 三层架构设计方案（Agent / Session / Query）

> 把 `xAgentSession` 现在身兼数职的单层状态机，拆成**三层架构**：
> **Agent Loop**（进程级自我）→ **Session Loop**（任务级对话）→ **Query Loop**（请求级无状态执行）。
>
> 本文档是**执行蓝本**：从职责切分到 API 草案到三步迁移路径全部给齐，后续直接按这份文档动手。

---

## 零、TL;DR

- **三层**职责正交——Agent 问 "这个 AI 怎么活"；Session 问 "这个任务怎么完成"；Query 问 "这次请求怎么跑完"。
- **数量关系**严格 1 : N : N——一个进程一个 Agent（通常），一个 Agent 持有 N 个 Session，一个 Session 内跑 N 次 Query。
- **近期落地**：Session/Query 拆分（三步走），对外 API 零 break。
- **远期登记**：Agent 层在 human-like-ai MVP 启动时开工；Session/Query 拆分时**预留勾子**供 Agent 将来接入，不堵死。
- **硬约束**：Query 层绝对无状态、绝对干净——Agent/Session 的上下文绝不通过 Query API 穿透。
- **硬前置**：Session/Query 拆分的触发条件是 human-like-ai MVP 决定启动；否则不启动。

---

## Part I · 架构定海神针

### 1. 三层切分的直觉

```text
┌──────────────── Agent Loop（进程级 / 用户级）─────────────────┐
│  "这个 AI 整体怎么活着"                                         │
│                                                                 │
│  ● 跨 session 自我认知（L2/L3 记忆、人格、风格偏好）            │
│  ● 长期记忆仓：用户稳定事实、项目约定、历史里程碑               │
│  ● 主动唤醒调度：定时器、事件触发、"想起来"的 push 通道         │
│  ● 多 session 共存管理：主线 session + sub-agent session       │
│  ● 人格一致性守卫：每个新开 session 的 system prompt 预算       │
│                                                                 │
│  持有：一份"自我" + N 个 Session                                │
└─────────────────────────────────────────────────────────────────┘
                            ↓ 管理 N 个
┌──────────────── Session Loop（任务级 / 对话级）───────────────┐
│  "这个任务/对话怎么推进到完成"                                  │
│                                                                 │
│  ● 本次对话完整 context（history + 工作记忆 L0/L1）             │
│  ● Context 压缩 / summarize                                    │
│  ● System prompt 组装（含从 Agent 层拉来的人格/记忆前缀）       │
│  ● Turn 预算、stop 决策                                        │
│  ● Sub-agent 编排（tool 里 spawn 子 Session）                   │
│  ● Memory 抽取：把本轮产出过筛后上报给 Agent                    │
│  ● 决定下一次 Query 的 input（用户新输入 / 自发总结 / 子任务）  │
│                                                                 │
│  持有：history + 若干次 Query 的生命周期                        │
└─────────────────────────────────────────────────────────────────┘
                            ↓ 一次任务里跑 N 次
┌──────────────── Query Loop（请求级 / 无状态）─────────────────┐
│  "把这次 LLM 请求跑到没有未 resolve 的 tool_use 为止"            │
│                                                                 │
│  ● submit → 流事件聚合 → tool dispatch → 再 submit             │
│  ● per-round scratch（text/thinking/tool_use buffer）          │
│  ● 不知道 history、不知道 memory、不知道兄弟 session            │
│                                                                 │
│  持有：一次 query 的临时 scratch                                │
└─────────────────────────────────────────────────────────────────┘
```

**三个问题完全正交**——这是切对了的标志。每一层的输入是上一层的决策，输出是一个可消化的 result。

### 2. 三层之间的关键协议

这一节划**协议**——层与层边界上"谁必须知道谁在做什么"。字段和 API 形态等实装前再定细节。

#### 2.1 Agent → Session：**注入**

Agent 在**每次 Session 创建时**提供三样东西：

1. **人格描述 / 风格约束**：注入到 Session 的 system prompt。内容稳定、跨 Session 一致、由 Agent 持有唯一来源。
2. **记忆前缀**（L2/L3 相关条目）：Agent 根据本次 Session 的类型/意图挑选相关记忆，打包成一段结构化上下文塞进 system prompt。**Session 不反向查询 Agent 的记忆仓**——避免 Session 层需要理解记忆索引。
3. **Mood 初始值**（v1 之后）：从 Agent 当前 mood state 拷一份给 Session 作为初始 mood，Session 内部可以演化这份 mood，结束时 Agent 再消化更新。

#### 2.2 Session → Agent：**上报**

Session 在**每次 Query 结束后**上报候选：

1. **L1 抽取候选**：从本轮 assistant 产出里过筛出"值得记住的东西"。**抽取在 Session 层做**（它最清楚这轮讲了啥），**落盘决策在 Agent 层做**（它最清楚全局，能去重 / 合并 / 冲突裁决）。
2. **Mood delta**（v1 之后）：本轮对话让用户/AI 的 mood 发生了什么变化。结构化的 delta，不是 free-form 文本。
3. **Session 生命周期事件**：被创建、被销毁、被用户主动结束、因错误终止。Agent 根据这些做"记忆固化"、"长期统计"等副作用。

#### 2.3 为什么"抽取—上报—裁决"是硬要求

如果把 L1 落盘也放在 Session 层，**每个 Session 写自己的一份持久化记忆**，会有两个问题：

- **多 Session 并存时的写冲突**：Session A 和 Session B 同时上报"麦伯伯偏好 tab 不用 space"，Agent 层能看到是重复事实直接去重；Session 层各写各的就会留两条。
- **全局视野缺失**：某条 L1 事实在单个 Session 里看价值一般，但**跨 Session 反复出现 5 次**才显出它是稳定事实。去重计数这件事必须要有"看得到全局"的层做。

这两件事只能由 Agent 做。所以"Session 抽取、Agent 裁决"是硬要求，不是风格选择。

#### 2.4 Agent → Agent（主动唤醒）：**自举**

主动唤醒场景下：

- Agent 的调度器（定时/事件）决定"该起一个新 Session 了"
- Agent 生成 initial input（"你想到什么就说什么" / "用户昨天说累了，主动问候一句"）
- Agent 创建 Session，Session 跑起来像普通对话一样
- 唯一区别是"第一条 input 不是用户发的，是 Agent 自发的"

这个路径要求 Session 层的 API **不假设 input 必须来自用户**——这是 Session/Query 拆分时必须预留的勾子。见 §8.2。

### 3. 记忆分层归属终稿

| 层级 | 内容 | 归属 | 生命周期 |
| --- | --- | --- | --- |
| **L0** | 当前对话的 raw history（turns、tool calls、events） | **Session** | 随 Session 销毁 |
| **L1** | 本 Session 内抽取的要点 | **Session 抽取、Agent 裁决落盘** | 跨 Session 存活、但会衰减/合并 |
| **L2** | 稳定事实（用户偏好、项目约定、关键里程碑） | **Agent** | 长期存在、定期 compact |
| **L3** | 自我认知与人格 | **Agent** | 近乎永久、极慢演化 |

**L1 在 Session 的角色**是"候选池"：Session 边跑边往里塞候选，跑完后一次性喂给 Agent，Agent 决定哪些进 L2、哪些归并到既有 L2、哪些直接丢弃。Session 自己不保留 L1——Session 销毁时 L1 随之消失，**留给后人看的 L1 必须已经通过裁决升级为 L2**，这条规矩能强迫 Agent 裁决不得偷懒。

---

## Part II · Session / Query 拆分（近期要执行）

### 4. 为什么要拆：现状痛点

#### 4.1 session.c 876 行里挤了 7 类职责

| # | 职责 | 代码位置 |
| --- | --- | --- |
| 1 | 滚动历史存储（flat entries + 折叠成 message） | `history_*`、`view_build` |
| 2 | 流式事件聚合（text / thinking / tool_use buffer） | `assist_*`、`reasoning_*`、`pending_*` |
| 3 | Tool loop（判 ToolUse → dispatch → 再 submit） | `on_provider_done` 后半段 |
| 4 | Turn 预算管理（max_turns、cancel、状态机） | `submit_round` + `finish_run` |
| 5 | Usage 跨轮累加（-1 哨兵） | `usage_accumulate` |
| 6 | 终止原因翻译（provider stop → done reason） | `translate_terminal` |
| 7 | Callback 路由（session-level callback → 外部） | `s->cbs.on_*` |

其中 **3 + 4** 已经在 `on_provider_done` 里缠成一团：判断 "这次停了之后要不要继续" 的那条 if 链，同时含了 **provider 终止原因、用户 cancel、max_turns、dispatch rc、cancel 二次检查、submit rc** 六种信号，70 多行。再往里塞 memory / compression / sub-agent 的 hook，就会变成"谁都不敢动"的地狱函数。

#### 4.2 现有架构无法干净容纳的特性

- **Context 压缩 / budget 管理**：`context_budget` 字段占了位但 `submit_round` 没真用它。压缩的天然时机是"两轮 LLM 请求之间"，但现状下"两轮之间"没有明确的回调/状态点。
- **Memory hook**（human-like-ai MVP 的核心）：L0/L1 抽取要在"这次对话终结后、下次开始前"做；L2/L3 注入要在"下次 submit 之前"做。同样需要"turn 边界"。
- **Sub-agent**：父 Session 的某个 tool handler 里 spawn 子 Session，await 子 Session 的最终回复并把它当 tool_result 塞回父 Session 的 history。现状下没有"我发起一次 query 并等它结束"的语义。
- **非流式一次性 query**（未来可能的批处理接口）：需要一个纯执行型的抽象。

#### 4.3 为什么叫 Query 不叫 Turn

- **"Turn"** 在 LLM 语境里通常指 user↔assistant **交替的轮次**。我们这个类型的本质是"一次查询产生若干 tool round 直到稳定"，用 Turn 会让读代码的人误以为它对应一次 user message ↔ assistant reply 的配对。
- **"Query"** 更贴合"一次调用 LLM（及其内部 tool loop）直到终结状态"的语义。
- 和 Claude Code 源码/文档的术语对齐（CC 的 `query()` 是无状态 generator 执行器），日后对照阅读零翻译成本。
- 内部静态函数前缀 `query_*` 比 `turn_*` / `q_*` 更自解释，读起来也不会和 `history_append_*` 这类动词打架。

### 5. 职责重新切分

| 原 session 职责 | 拆后归属 |
| --- | --- |
| 1 滚动历史存储 | **Session** |
| 2 流式事件聚合 | **Query**（一次 query 的 scratch） |
| 3 Tool loop | **Query**（query 的本质） |
| 4 Turn 预算（max_turns） | **Session**（决策边界） |
| 5 Usage 累加（跨 query） | **Session** |
| 6 Stop reason 翻译 | **Query** 生成 result，**Session** 翻译给用户 |
| 7 Callback 路由 | 两层各有，外层透传 |
| 8 Context 压缩（新） | **Session**（query 间） |
| 9 Prompt 注入（新） | **Session**（query 前构造） |
| 10 Memory hook（新） | **Session**（query 间） |
| 11 Sub-agent（新） | **Session**（起子 Session） |

### 6. 两层协作示意

```text
┌─────────────────── xAgentSession（长期持有、有状态）────────────────────┐
│                                                                       │
│  history[], agent, memory, budget, max_turns, cbs…                   │
│                                                                       │
│  for (;;) {                                                          │
│    view = build_view(history + system_prompt + memory_prefix);        │
│    xAgentQuery q = xAgentQueryCreate(sess, &forwarding_cbs);               │
│    xAgentQueryRun(q, view, next_input);                                  │
│    ...（等 on_done(result) 回调）...                                   │
│                                                                       │
│    // ↓↓↓ 以下三件事是 "Query 之间" 做的，跟 Query 内部零耦合 ↓↓↓     │
│    memory_absorb(sess, result);   // L0/L1 抽取                       │
│    maybe_compact(sess);           // budget 预警时压缩                │
│    next_input = decide_next(sess);// 继续 / 结束 / 起 sub-agent       │
│  }                                                                    │
└───────────────────────────────────────────────────────────────────────┘
                         ↓ 每一轮 create 一个
┌─────────────────── xAgentQuery（短命、无状态、一次性）──────────────────┐
│                                                                       │
│  从 Session 借来 view + input，自己内部跑 tool loop：                │
│    submit → stream events → 若 ToolUse：dispatch tools → 再 submit    │
│    → 直到 provider 返回非 ToolUse 的终结状态（Terminal/Error/Cancel） │
│                                                                       │
│  对外只流式 yield 四类事件，最后给一次 on_done(result)：              │
│    on_text / on_thinking / on_tool / on_done(xAgentQueryResult*)         │
│                                                                       │
│  不知道 memory、不知道 compact、不知道 session 历史                   │
└───────────────────────────────────────────────────────────────────────┘
```

### 7. 新 API 草案

```c
/* ── xagent/query.h ────────────────────────────────────────────────── */

XDEF_HANDLE(xAgentQuery);

/**
 * 一次 query 的最终结果，on_done 时交给调用方（通常是 session.c）。
 * 指针只在回调期间有效，Session 消化完就释放。
 */
XDEF_STRUCT(xAgentQueryResult) {
  xAgentProviderStopReason stop_reason; /* 最后一轮 provider 给的原因   */
  xErrno                err;         /* 若 stop_reason == Error      */
  xAgentUsage              usage;       /* 这次 query 跨所有 round 的累加 */

  /* query 期间 append 到 session history 的条目范围 [begin, end)。
   * Session 用这个区间做 memory_absorb / compact 的输入界定。    */
  size_t hist_begin;
  size_t hist_end;

  int    rounds;                     /* 本次 query 实际的 provider
                                        submit 次数（>= 1）         */
};

XDEF_STRUCT(xAgentQueryCallbacks) {
  void (*on_text)    (xAgentQuery q, const char *chunk, size_t len, void *ud);
  void (*on_thinking)(xAgentQuery q, const char *chunk, size_t len, void *ud);
  void (*on_tool)    (xAgentQuery q, const char *tool_name, int started, void *ud);
  void (*on_done)    (xAgentQuery q, const xAgentQueryResult *result, void *ud);
  void *user_data;
};

/**
 * 配置：query 执行过程中的 *局部* 限制，不涉及 memory/compact 等
 * 外层决策性参数——那些留给 Session 层。
 */
XDEF_STRUCT(xAgentQueryConf) {
  int max_rounds;   /* 本次 query 内 tool loop 最多几轮 submit；
                       0 = 继承 session->max_turns                  */
  int max_tokens;   /* 每轮 submit 的 completion 上限；0 = 继承     */
};

XCAPI(xAgentQuery)
xAgentQueryCreate(xAgentSession sess,
               const xAgentQueryConf *conf,
               const xAgentQueryCallbacks *cbs);

/**
 * 启动。输入 input 会被 append 到 session history，然后向 provider
 * 提交第一次 submit。query 从此进入自循环直到 on_done。
 *
 * 调用方应确保：
 *  - Session 当前没有别的 query 在跑（由 session 层保证）
 *  - input 的内存所有权规则与 xAgentSessionInput 一致（shallow copy）
 */
XCAPI(xErrno) xAgentQueryRun(xAgentQuery q, xAgentMessage input);

/** 请求取消；on_done 仍会 fire（stop_reason == Cancelled）。 */
XCAPI(void) xAgentQueryCancel(xAgentQuery q);

/** 销毁。若还在跑，内部先 cancel 并 drain 完回调再释放。 */
XCAPI(void) xAgentQueryDestroy(xAgentQuery q);
```

**Session 的变化面**（对现有 `xAgentSession` API）：

- `xAgentSessionInput(sess, msg)` 的**签名不变**，内部实现改成 "创建一个 xAgentQuery 并启动它"。
- `xAgentSessionCallbacks` 的 `on_text / on_thinking / on_tool / on_done / on_error` **保持不变**。Session 内部做一层 forwarding：query 的回调先进 Session，Session 加工一下（比如 on_done 要翻译 stop_reason 成 xAgentDoneReason、加上跨 query 累加的 usage）再抛给用户。
- **对外 API 零 break**。所有改动都是内部重构。

### 8. Agent 层对 Session/Query 拆分的反向约束

Agent 层现在不动手，但 Session/Query 拆分时**必须留几个勾子**，否则将来引入 Agent 会二次大改 Session API。

#### 8.1 Session 的 callback 分发不硬编码单消费者

Session 现在对外暴露的 `xAgentSessionCallbacks` 假设**用户代码**是唯一消费者。Agent 层上来之后，callback 的消费者会变成 **"Agent + 用户代码"** 双路。

- **落实**：Session 拆分阶段**保留**现有 callback API 给外部用户；Agent 层将来通过**另一条内部观察者接口**接入，不走公开 callback。
- **含义**：Session 内部的事件分发不要硬编码"只 fan-out 到一个 callbacks 结构"，留一个可扩展的 observer list（或至少预留 `void *owner; void (*on_event)(...)` 这种钩子槽位）。

#### 8.2 Session 的 input 显式携带 origin 标记

Session 现在的 `xAgentSessionInput` 隐含"user message"语义。Agent 主动唤醒场景下，initial input 是 Agent 合成的。

- **落实**：Session 拆分时就把 input 定义**显式携带一个 origin 标记**（`user` / `system_synthesized`），而不是靠调用路径隐式区分。
- **含义**：Agent 层引入后，内部 system-synthesized input 不会污染 L1 抽取（Session 知道"这条不是用户说的，别当成用户偏好"）。

#### 8.3 Session 销毁要有"可上报"的钩子

Session 销毁时 Agent 需要做一次 final digest——把还没上报的 L1 候选、mood delta 汇总一次。

- **落实**：Session 销毁流程里预留一个 `on_session_finalizing` 回调点，在资源释放**之前**调用。
- **含义**：Agent 将来挂进去只需要实现这个回调，不需要改 Session 销毁流程。

#### 8.4 Query 层保持绝对无状态、绝对干净【最硬规矩】

Agent 层的任何勾子都**不应该**穿透到 Query 层。Query 层不感知有没有 Agent，也不感知 memory、mood、sub-agent。这是三层解耦最硬的规矩。

- **落实**：Query 的所有 callback 参数只带"这一次查询"的数据，绝不带 session/agent 指针。需要 session/agent 上下文的特性（比如 tool handler 想查 memory），通过 session 层的 `user_data` 透传，**不改 Query API**。

### 9. Callback 透传层的设计

Session 内部维护一个 per-session 的 `xAgentQueryCallbacks`，每次起 Query 时传给它：

```c
static void forward_on_text(xAgentQuery q, const char *chunk, size_t len, void *ud) {
  struct xAgentSession_ *s = ud;
  if (s->cbs.on_text) s->cbs.on_text((xAgentSession)s, chunk, len, s->cbs.user_data);
}
/* forward_on_thinking / forward_on_tool 同理 */

static void forward_on_done(xAgentQuery q, const xAgentQueryResult *r, void *ud) {
  struct xAgentSession_ *s = ud;

  /* 跨 query 累加 usage */
  session_usage_accumulate(s, &r->usage);

  /* Query 间 hook 点 —— MVP 阶段先留空，未来 memory/compact 接入 */
  /* memory_absorb(s, r); */
  /* maybe_compact(s);    */

  xAgentDoneReason reason = session_translate_stop(r->stop_reason, s->cancelled);
  if (reason == xAgentDoneReason_ModelError && s->cbs.on_error) {
    s->cbs.on_error((xAgentSession)s, r->err, NULL, s->cbs.user_data);
  }
  if (s->cbs.on_done) {
    s->cbs.on_done((xAgentSession)s, reason, &s->usage, s->cbs.user_data);
  }

  /* 释放 query */
  xAgentQueryDestroy(s->current_q);
  s->current_q = NULL;
  s->running   = 0;
}
```

这层 forwarding 本身**就是** Session 作为 "Agent Loop" 的第一个雏形——它已经有了"在 Query 之间干一点事"的能力。

### 10. 迁移路径：三步走

每步可独立 PR、独立 review、独立回滚。

#### Step 1：内部静态函数族分组（不拆类型、不拆文件）

只做 `session.c` 内部的函数重排，目标是让"agent 层决策"和"query 层执行"在同一个文件里**可视化地分开**。

具体动作：

- 把 `submit_round` / `on_provider_*` / `assist_*` / `reasoning_*` / `pending_*` / `view_build` 等函数，**重命名**为 `query_submit_round` / `query_on_provider_*` / `query_assist_*` …
- 把 `history_*` / `commit_assistant_turn` / `finish_run` / `translate_terminal` / `usage_accumulate` 留为 `session_*` 或不前缀（表示"决策层"）。
- `on_provider_done` 拆成 3 个小函数：`query_handle_error()`、`query_handle_tool_loop_continuation()`、`query_handle_terminal()`，原函数变成只做三路分派的 3-5 行调度器。
- **对外 API、public header、测试全部不动**。纯物理重组。

产物：一个 PR，`session.c` diff 大但语义零变化，`npm test` 全绿。

#### Step 2：正式引出 xAgentQuery 类型

- 新建 `modules/xagent/query.h`、`query_private.h`、`query.c`、`query_test.cpp`。
- 把 Step 1 里 `query_` 前缀的那批函数 + 相关数据（`assist_buf` / `reasoning_buf` / `pending` / `turn`）**搬家**到 `query.c`。
- `struct xAgentSession_` 瘦身：删掉那些搬走的字段，加一个 `xAgentQuery current_q` 字段。
- `session.c` 的 `xAgentSessionInput` 改写成 `QueryCreate + QueryRun` 两步。
- **同步落实 §8.1 / §8.2 / §8.3 三条 Agent 预留勾子**：
  - Session 内部事件分发走 observer list（即使当前只有一条用户 callback 作为 observer）
  - `xAgentSessionInput` 内部把 input 显式标记为 `user_origin`
  - 预留 `on_session_finalizing` 回调槽
- 新增 `query_test.cpp`：脱开 Session 独立测 Query（需要一个轻量 fake session，仅暴露 history append + provider）。原 `session_test.cpp` 的 fake_submit 改造成 fake_query，测 Session 层的 forwarding + usage 累加 + cancel。

产物：一个 PR，代码面净增（Query 独立测试），Session 净减。对外 API 仍然零 break。

#### Step 3（可选）：把 Query 做成可独立使用的

在 Step 2 后，Query 其实**已经不依赖** Session 的任何独特能力（只依赖 agent、history 引用、provider）。可以开放 `xAgentQueryCreateStandalone(agent, view, ...)` 给不需要 Session 长期状态的调用方用——例如批处理脚本、单次 QA 工具。

**不是必须**。只有遇到"某个用户确实想用 query loop 但不想要 session"的真实需求才做。

### 11. 对测试的影响

#### 11.1 现状盘点

```text
modules/xagent/session_test.cpp     — 覆盖 session-level 的 Input/Cancel/Destroy、
                                    tool loop、max_turns、cb_done 签名
modules/xagent/provider_openai_test.cpp — 覆盖 provider wire 编解码
modules/xagent/agent_test.cpp       — agent 级 tool 注册 / 生命周期
modules/xagent/tool_test.cpp        — tool 对象本身
modules/xagent/message_test.cpp     — message 结构
```

#### 11.2 改造量预估

| 文件 | 改造内容 | 工作量 |
| --- | --- | --- |
| `session_test.cpp` | fake_submit 改成 fake_query，验 Session 层 forwarding & usage 累加 | **大**（≈ 60% 重写） |
| `query_test.cpp` | **新增**：fake_provider + 独立测 tool loop / reasoning / pending / cancel | 从零 |
| `provider_openai_test.cpp` | 零改（Query 和 provider 的契约没变） | 0 |
| `agent_test.cpp` / `tool_test.cpp` / `message_test.cpp` | 零改 | 0 |

粗估 2-3 个整天的测试重构。

#### 11.3 Step 1 / Step 2 的风险缓冲

- **Step 1 是纯物理重组**，session_test 不改而且必须全绿——这是 Step 2 能开始的前提。如果 Step 1 哪个 case 挂了就说明重组把语义动了，回滚。
- **Step 2 的 fake_query 要先设计好接口**，不要等到 session_test 改到一半才发现 fake 不够用。先用"最小 fake"（只能 done 一次、不支持 tool loop）跑通 Session 层最粗的 smoke test，再往 fake 里加能力。

#### 11.4 Addendum（2026-04-25）：fake_query 改造已关闭

事后复盘：§11.2 里预估的 "fake_submit → fake_query ≈ 60% 重写" 没有发生，也**不再计划发生**。原因是实际落地后 `session_test.cpp` 的形态已经满足当初要拆出 fake_query 时想达成的所有目标，不需要再做一轮机械替换。

具体来说：

1. **当前 `session_test.cpp` 事实上已是 Session + Query 集成测试**。fake provider 驱动真实的 `xAgentQuery` 执行链（tool loop、cancel、reasoning、usage），Session 层的 forwarding 契约全部用端到端断言覆盖，每个用例的 intent 清晰——并没有"混在一起测不准"的问题。硬塞一个 fake_query 反而会把这条回归链路切断。
2. **Query 的白盒覆盖由新增 `query_test.cpp` 独立承担**（见 `879d895`）。Query 状态机、observer 派发、history 解耦这些点的单元测试责任已经从 Session 测试里析出了，不再需要通过 "fake_query" 反向模拟。
3. **`SubmitFailureRollsBackAndReturnsError` 等用例已经在直接断言 `s->query == nullptr`**——说明 session_test 已经感知 Query 的生命周期，早已不是 §11.1 盘点时那个 "只看 provider 黑盒" 的形态。

结论：本条从 §12 开工清单撤下（标记为已关闭，非已完成）；后续若真的出现 "fake provider 层难以驱动某个 Session 决策路径" 的用例，再按需引入 fake_query，届时对 `session_test.cpp` 也只需要增量补测、不是重写。

### 12. 开工清单

- [x] **Step 1**：`session.c` 内部 `query_*` / `session_*` 分组重命名 + `on_provider_done` 拆三份
- [x] **Step 1**：`npm test` 9/9 全绿验证
- [x] **Step 1**：PR 提交 + self-review 确认 diff 零语义变化
- [x] **Step 2**：新增 `query.h/c/private.h`，从 Session 搬运字段与函数
- [x] **Step 2**：`xAgentSession_` 瘦身，持有 `xAgentQuery current_q`
- [x] **Step 2**：落实 §8.1 observer list、§8.2 input origin 标记、§8.3 `on_session_finalizing` 勾子
- [x] **Step 2**：新增 `query_test.cpp`（含 fake_provider）
- [~] **Step 2**：~~`session_test.cpp` 改造 fake_submit → fake_query~~ — **已关闭，见 §11.4**。`session_test.cpp` 当前已等价承担 Session + Query 集成测试，不再需要此改造。
- [x] **Step 2**：`npm test` 全绿 + `xagent_test` 通过
- [ ] **Step 2**：更新 `docs/xagent-module.md`（如果有的话）说明新的双层结构
- [ ] **Step 3**（可选）：开放 `xAgentQueryCreateStandalone`，文档里给一个批处理 use case

---

## Part III · Agent 层（远期登记）

### 13. 为什么 Agent 层不能并入 Session

有个合理的反问：**Session 本来就是一个"对话"的抽象，跨对话的事交给进程/主程序不就行了？**——如果只做 Part II 的 Session/Query 拆分，确实不需要 Agent 层。Agent 层的**必要性完全来自 human-like-ai 规划的四个维度**。

| 维度 | 为什么必须 Agent 层 |
| --- | --- |
| **分层记忆 L2/L3** | L2 是跨 session 的稳定事实，L3 是长期自我认知。归属权必须在所有 Session 之上，否则每一次 Session 生死都会拖一个 L2/L3 全量 I/O，还容易写冲突。 |
| **情绪延续** | Mood 必须在 Session 边界之外 carry-over，否则每新开一个对话都是冷启动情绪。只有一个常驻的"自我"才能持有 mood state。 |
| **主动唤醒** | 定时器/事件触发时，**当下可能根本没有活跃 Session**。由 Agent 层决定"要不要起一个新 Session"以及"input 是什么"。Session 层无法自举。 |
| **人格一致性** | 每新开一个 Session，system prompt 要注入一致的人格描述。如果让每个 Session 自行维护人格字符串，无法保证一致（也难以升级、AB test 不同人格版本）。 |
| **Sub-agent 并存** | 父 Session 在 tool 里 spawn 子 Session，两者谁来 own？放在父 Session 里就成了"Session 持有 Session"，生命周期纠缠；放在 Agent 层就是"Agent 持有 N 个 Session，其中两个有父子关系"，干净。 |

**如果这四件事都不做**，Agent 层就是过度设计。**如果这四件事里有任何一件认真做**，Agent 层就不可省略。

### 14. Agent 层开工范围（提纲，未到日不细写）

- [ ] 定 `xAgent` opaque handle + 核心 struct 字段（memory store、mood、scheduler、session list）
- [ ] 实装 Agent → Session 注入（人格前缀、记忆前缀）
- [ ] 实装 Session → Agent 上报（L1 抽取回调、session_finalizing 回调）
- [ ] 实装 L2/L3 的持久化后端（选型：sqlite? 文本? 文件布局？——独立起一份 `docs/design/xagent_memory_storage.md`）
- [ ] 主动唤醒调度器（先做一个最简单的定时器 MVP）
- [ ] Mood state（v1，不在 MVP 内）
- [ ] 示例 `examples/ai_agent.cpp`（像 `apps/cli` 一样的 REPL，但持有 Agent）
- [ ] 测试：`agent_test.cpp` 扩展 + `session_agent_integration_test.cpp`

### 15. Agent 层开放问题

#### 15.1 Process singleton 还是允许多实例？

**倾向**：**不强制 singleton**。一个进程可以创建多个 `xAgent`（每个绑定不同用户身份），但**常见用法是一个进程一个 Agent**。这样设计测试友好（可以在同进程里并行测多个 agent），也方便未来做 multi-tenant。

#### 15.2 L2/L3 持久化格式

初期考虑：

- JSON Lines 文件（易调试、易手工修）
- SQLite（查询灵活、但依赖更重）
- 先 JSONL MVP、v1 再迁 SQLite？

不在本文档决定，真正做到那一步时单独起一份 `docs/design/xagent_memory_storage.md`。

#### 15.3 并发模型

Agent 需要持有多个 Session、需要响应定时器事件——它**一定**是运行在 xEventLoop 之上的。

- Agent 绑定一个 loop，Session 必须绑定同一个 loop，这是最简单的模型。
- 跨 loop 的 Agent/Session 暂不考虑——有需求时再说，不提前抽象。

#### 15.4 Mood 的表示

v1 之后的事，先不管。但脑子里要有个粗草案：不是连续浮点（难解释难 debug），是**离散状态 + 辅助度量**——比如 `{tone: calm/tired/excited, energy: 0..3}` 这类小集合。

#### 15.5 主动唤醒的用户体验

这是产品问题不是架构问题，但本层要提供"**用户可关闭/降频**"的开关。默认行为应该**保守**——宁可错过主动时机也不要乱刷屏。

---

## Part IV · 执行时机与风险

### 16. 总体时间线

```text
  now                                          future
   │                                              │
   ├── human-like-ai MVP 决定启动 ──────────────┐│
   │                                            ↓↓
   ├── Step 1: session.c 内部分组（纯物理）──┐  │
   │   ● 对外 API 零 break                    │  │
   │   ● npm test 9/9 全绿                    │  │
   │                                          ↓  │
   ├── Step 2: 正式引入 xAgentQuery ────────────┐│  │
   │   ● 落实 Agent 层预留勾子（§8.1~8.3）  ││  │
   │   ● fake_query 最小 MVP 先跑通 smoke   ││  │
   │   ● session_test 60% 重写              ↓│  │
   │                                         │  │
   ├── Step 3（可选）: standalone Query ────┘  │
   │                                            │
   ├── human-like-ai MVP 开工 ─────────────────┤
   │   ● 引入 xAgent handle                  │
   │   ● 记忆 L2/L3 持久化                     │
   │   ● 主动唤醒调度器                        │
   │                                           │
   └── v1 / v2：情绪延续、选择性遗忘、主动唤醒升级 ─┘
```

### 17. 风险总览

| 项 | 评估 | 缓解 |
| --- | --- | --- |
| **对外 API break** | 无。所有改动内部。 | 三步都严守"对外 API 零 break"硬约束 |
| **行为回归** | Step 1 纯物理重组风险最低；Step 2 动了数据字段归属 | Step 1 必须 session_test 9/9 全绿才能进 Step 2；Step 2 先用最小 fake 跑通 smoke 再扩展 |
| **测试工作量** | 2-3 天测试重构 | Step 2 拆成多个小 commit 渐进，不要一次性堆完所有 case |
| **Agent 预留勾子设计不到位** | 将来引入 Agent 时要二次改 Session | Step 2 就落实 §8.1~8.3，不留到 Agent 开工再补 |
| **如果 human-like-ai 不做了** | Session/Query 拆分的主要价值消失 | 拆分是 MVP 前置条件，**MVP 决定启动时才启动拆分**；否则不启动 |
| **Query 层不干净** | 被 Agent 特性穿透，三层白分 | **§8.4 最硬规矩**：Query 所有 callback 参数只带本次查询数据，上下文通过 `user_data` 透传 |

### 18. 启动时机硬约束

- **Session/Query 拆分**的触发条件：**human-like-ai MVP 决定启动**。否则不启动——不是架构美观必需品。
- **Agent 层**的触发条件：Session/Query Step 2 完成 **且** human-like-ai MVP 进入"引入跨 session 记忆"阶段。

这两条约束必须严守。架构设计可以提前半年写好，但动手写代码要绑定真实产品需求。

#### 18.1 MVP 启动记录

- **2026-04-24**：human-like-ai MVP 启动扳机已扣下。拆分 MVP-a（L0+L1 + JSONL + Agent 层雏形）和 MVP-b（L2 + 向量 + SQLite）两小段，详见 [`human-like-ai.md` §6 MVP 执行边界](human-like-ai.md)。
- **因此 Session/Query 拆分 Step 1 解锁**，可以开工；Step 2 同期进行，为 MVP-a 的 Agent 勾子落地做准备。
- **Agent 层**的硬前置仍未满足——等 MVP-a 跑稳、确认要接 L2 跨 session 记忆后再启动。

---

## Part V · 附录

### 19. 三层命名速查

| 层 | 类型名 | 内部前缀 | 文件 | 职责一句话 |
| --- | --- | --- | --- | --- |
| Agent Loop | `xAgent`（将来） | `agent_*` | `agent.h/c`（将来） | "这个 AI 怎么活" |
| Session Loop | `xAgentSession`（现有） | `session_*` / 待梳理 | `session.h/c`（现有） | "这个任务怎么完成" |
| Query Loop | `xAgentQuery`（Step 2 后） | `query_*` | `query.h/c`（将来） | "这次请求怎么跑完" |

**命名一致性原则**：Agent 内部静态函数用 `agent_*`（模块短前缀去掉首字母 x → `agent`，规则和 `xfer → xfer_*` 一致）。

### 20. 与其他文档的关系

- **[human-like-ai.md](human-like-ai.md)**：产品方向，回答"做什么"。本文回答"做的东西住在哪"以及"近期怎么动手"。
- **未来**：
  - `docs/design/xagent_memory_storage.md`：L2/L3 存储选型（Agent 层开工时写）
  - `docs/design/xagent_agent_api.md`：Agent 公开 API 正式定义（Agent 层开工时写）

---

_作者：小W（与麦伯伯讨论后整理）_
_日期：2026-04-24_
_状态：execution plan / 已定稿，按此执行_
