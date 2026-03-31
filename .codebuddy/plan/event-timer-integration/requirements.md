# 需求文档：Event + Timer 集成

## 引言

当前项目中 `xEventLoop`（I/O 多路复用）和 `xTimer`（定时器）是两个独立模块。`xEventLoop` 通过 kqueue/epoll/poll 驱动 I/O 事件，`xTimer` 通过独立后台线程 + min-heap 驱动定时回调。两者各自运行，无法在同一个循环中统一调度 I/O 和定时任务。

本次需求将两个模块结合起来，提供三种集成方案，分别在独立分支上实现：

- **方案 A（轻量集成）**：分支 `codebuddy/event-timer-attach`。不改动 `xTimer` 和 `xEventLoop` 的内部实现，扩展 `xEventLoopCreate` 使其可选地接收一个 poll 模式的 `xTimer`，并新增 `xEventLoopRun` / `xEventLoopStop` 主循环 API，让 event loop 自动驱动 timer。
- **方案 B（深度集成）**：分支 `codebuddy/event-timer-builtin`。在 `xEventLoop` 内部直接嵌入 `xHeap`，提供 `xEventLoopTimerAfter` 等 API，实现单线程零锁的定时器调度。
- **方案 C（Timer 驱动）**：分支 `codebuddy/event-timer-driven`。让 `xTimer` 作为主驱动者，将 `xEventLoop` 绑定到 `xTimer` 的后台线程上。timer 线程在等待下一个 deadline 的间隙同时驱动 I/O 多路复用，实现单线程统一调度，复用现有 `xTimer` 的 heap 和线程管理。

三个方案均需编写完整的单元测试。

---

## 需求

### 需求 1：方案 A — 轻量集成（Attach 模式）

**用户故事：** 作为一名库使用者，我希望在创建 `xEventLoop` 时就能传入一个 poll 模式的 `xTimer`，并通过 `xEventLoopRun` 统一驱动 I/O 和定时任务，以便不需要手动管理两套独立的事件驱动机制。

#### 1.1 xTimer 新增最近 deadline 查询 API

##### 验收标准

1. WHEN 调用 `xTimerNextDeadline(timer)` THEN 系统 SHALL 返回 timer 内部 min-heap 堆顶元素的 deadline（绝对时间，毫秒），如果堆为空则返回 `UINT64_MAX`。
2. IF timer 为 NULL THEN `xTimerNextDeadline` SHALL 返回 `UINT64_MAX`。
3. WHEN 新的 timer task 被提交或取消后 THEN `xTimerNextDeadline` 的返回值 SHALL 反映最新的堆顶 deadline。

#### 1.2 xEventLoopCreate 扩展：可选传入 xTimer

##### 验收标准

1. WHEN 调用 `xEventLoopCreate(maxEvents, timer)` 且 timer 不为 NULL THEN 系统 SHALL 创建 event loop 并将该 timer 关联到 loop，后续 `xEventLoopRun` 会自动驱动该 timer。
2. WHEN 调用 `xEventLoopCreate(maxEvents, NULL)` THEN 系统 SHALL 创建一个不带 timer 的 event loop（行为与原来一致）。
3. IF timer 不是 poll 模式（即创建时 `g != NULL`）THEN `xEventLoopCreate` SHALL 返回 NULL 并设置错误码。
4. IF maxEvents <= 0 THEN `xEventLoopCreate` SHALL 返回 NULL（与原有行为一致）。

#### 1.3 xEventLoopRun / xEventLoopStop 主循环

##### 验收标准

1. WHEN 调用 `xEventLoopRun(loop)` THEN 系统 SHALL 进入阻塞主循环，每轮循环执行：
   - 计算 timeout = min(默认最大超时, `xTimerNextDeadline(timer) - now`)（如果已绑定 timer）
   - 调用 `xEventWait(loop, timeout)` 等待 I/O 事件
   - 调用 `xTimerPoll(timer)` 执行到期的定时回调（如果已绑定 timer）
2. WHEN 调用 `xEventLoopStop(loop)` THEN 系统 SHALL 设置停止标志并通过 `xEventWake` 唤醒阻塞中的 `xEventWait`，使 `xEventLoopRun` 返回。
3. IF 创建时未传入 timer THEN `xEventLoopRun` SHALL 仍然正常工作，仅驱动 I/O 事件。
4. IF loop 为 NULL THEN `xEventLoopRun` 和 `xEventLoopStop` SHALL 直接返回（不崩溃）。

#### 1.4 方案 A 单元测试

##### 验收标准

1. WHEN 编写测试 THEN 测试 SHALL 覆盖以下场景：
   - `xTimerNextDeadline` 空堆返回 `UINT64_MAX`
   - `xTimerNextDeadline` 返回正确的堆顶 deadline
   - `xEventLoopCreate` 传入 poll 模式 timer 成功创建
   - `xEventLoopCreate` 传入 push 模式 timer 返回 NULL
   - `xEventLoopCreate` 传入 NULL timer 正常创建（向后兼容）
   - `xEventLoopRun` 同时处理 I/O 事件和定时回调
   - `xEventLoopRun` 在未传入 timer 时仅处理 I/O
   - `xEventLoopStop` 能正确终止主循环
   - 定时回调的延迟精度在合理范围内（误差 < 50ms）
   - NULL 参数不崩溃

---

### 需求 2：方案 B — 深度集成（Builtin 模式）

**用户故事：** 作为一名库使用者，我希望能直接在 `xEventLoop` 上注册定时回调，无需额外创建 `xTimer` 对象，以便获得单线程、零锁、最低延迟的定时器调度。

#### 2.1 xEventLoop 内嵌 timer heap

##### 验收标准

1. WHEN 创建 `xEventLoop` THEN 系统 SHALL 在内部初始化一个 `xHeap` 用于管理定时任务。
2. WHEN 销毁 `xEventLoop` THEN 系统 SHALL 释放 heap 中所有未到期的定时任务（回调不执行），并销毁 heap。

#### 2.2 xEventLoopTimerAfter API

##### 验收标准

1. WHEN 调用 `xEventLoopTimerAfter(loop, fn, arg, delay_ms)` THEN 系统 SHALL 创建一个定时任务并插入 heap，返回一个 `xEventTimer` 句柄。
2. IF fn 为 NULL 或 loop 为 NULL THEN 系统 SHALL 返回 NULL。
3. WHEN delay_ms 为 0 THEN 定时任务 SHALL 在下一次 `xEventWait` 返回时立即执行。

#### 2.3 xEventLoopTimerAt API

##### 验收标准

1. WHEN 调用 `xEventLoopTimerAt(loop, fn, arg, abs_ms)` THEN 系统 SHALL 创建一个定时任务，deadline 为 abs_ms，插入 heap 并返回句柄。
2. IF abs_ms 已经过期 THEN 定时任务 SHALL 在下一次 `xEventWait` 返回时立即执行。

#### 2.4 xEventLoopTimerCancel API

##### 验收标准

1. WHEN 调用 `xEventLoopTimerCancel(loop, timer)` THEN 系统 SHALL 从 heap 中移除该定时任务并释放内存，返回 `xErrno_Ok`。
2. IF timer 已经到期执行过 THEN 系统 SHALL 返回 `xErrno_Unknown`。
3. IF loop 或 timer 为 NULL THEN 系统 SHALL 返回 `xErrno_Unknown`。

#### 2.5 xEventWait 集成 timer 到期检查

##### 验收标准

1. WHEN 调用 `xEventWait(loop, timeout_ms)` 且 heap 非空 THEN 系统 SHALL 将实际超时时间设为 `min(timeout_ms, heap_top_deadline - now)`。
2. WHEN `xEventWait` 返回后 THEN 系统 SHALL 检查 heap 并 pop + fire 所有已到期的定时任务（deadline <= now），在 I/O 回调之后执行。
3. WHEN 定时任务到期并执行 THEN 系统 SHALL 不将其计入 `xEventWait` 的返回值（返回值仅统计 I/O 事件数）。
4. IF timeout_ms 为 -1（无限等待）且 heap 非空 THEN 实际超时 SHALL 被 clamp 到最近的 deadline。

#### 2.6 xEventLoopRun / xEventLoopStop 主循环

##### 验收标准

1. WHEN 调用 `xEventLoopRun(loop)` THEN 系统 SHALL 进入阻塞主循环，反复调用 `xEventWait(loop, -1)` 直到 `xEventLoopStop` 被调用。
2. WHEN 调用 `xEventLoopStop(loop)` THEN 系统 SHALL 设置停止标志并唤醒 event loop，使 `xEventLoopRun` 返回。
3. IF loop 为 NULL THEN `xEventLoopRun` 和 `xEventLoopStop` SHALL 直接返回。

#### 2.7 跨线程安全

##### 验收标准

1. WHEN 从非 event loop 线程调用 `xEventLoopTimerAfter` / `xEventLoopTimerAt` THEN 系统 SHALL 通过互斥锁保护 heap 操作，并调用 `xEventWake` 唤醒阻塞中的 `xEventWait` 以重新计算超时。
2. WHEN 从非 event loop 线程调用 `xEventLoopTimerCancel` THEN 系统 SHALL 通过互斥锁保护 heap 操作。

#### 2.8 方案 B 单元测试

##### 验收标准

1. WHEN 编写测试 THEN 测试 SHALL 覆盖以下场景：
   - `xEventLoopTimerAfter` 基本延迟触发
   - `xEventLoopTimerAfter` delay=0 立即触发
   - `xEventLoopTimerAt` 绝对时间触发
   - `xEventLoopTimerAt` 过期 deadline 立即触发
   - `xEventLoopTimerCancel` 成功取消
   - `xEventLoopTimerCancel` 对已执行的 timer 返回错误
   - 多个 timer 按 deadline 顺序执行
   - timer 和 I/O 事件混合调度
   - `xEventLoopRun` + `xEventLoopStop` 主循环
   - 跨线程提交 timer 并唤醒 event loop
   - 销毁 loop 时丢弃未到期 timer（不执行回调）
   - NULL 参数不崩溃
   - 定时回调的延迟精度在合理范围内（误差 < 50ms）

---

### 需求 3：方案 C — Timer 驱动（Timer-Driven 模式）

**用户故事：** 作为一名库使用者，我希望将 `xEventLoop` 绑定到 `xTimer` 上，让 timer 的后台线程同时驱动 I/O 事件和定时任务，以便复用 `xTimer` 已有的线程和 heap 管理能力，用最少的改动实现统一调度。

#### 3.1 xTimer 绑定 xEventLoop

##### 验收标准

1. WHEN 调用 `xTimerCreate(g)` 创建 timer 后，再调用 `xTimerAttachEventLoop(timer, loop)` THEN 系统 SHALL 将 event loop 绑定到 timer 的后台线程，timer 线程在等待 deadline 的同时驱动 I/O 多路复用。
2. IF timer 为 NULL 或 loop 为 NULL THEN `xTimerAttachEventLoop` SHALL 返回 `xErrno_Unknown`。
3. IF timer 已经绑定了一个 event loop THEN `xTimerAttachEventLoop` SHALL 返回 `xErrno_Unknown`（不支持重复绑定）。
4. WHEN 绑定成功后 THEN timer 后台线程 SHALL 将原来的 `pthread_cond_timedwait` 等待替换为 `xEventWait(loop, timeout)` 等待，其中 timeout 由最近的 heap 顶 deadline 决定。

#### 3.2 timer 线程驱动模型变更

##### 验收标准

1. WHEN timer 未绑定 event loop THEN timer 后台线程 SHALL 保持原有行为（`pthread_cond_timedwait` 等待 deadline）。
2. WHEN timer 已绑定 event loop THEN timer 后台线程每轮循环 SHALL 执行：
   - 计算 timeout = heap 为空 ? -1 : max(0, heap_top_deadline - now)
   - 调用 `xEventWait(loop, timeout)` 同时等待 I/O 事件和 timer 到期
   - 检查 heap 并 fire 所有已到期的定时任务
3. WHEN 跨线程提交新 timer task（`xTimerSubmitAfter` / `xTimerSubmitAt`）THEN 系统 SHALL 调用 `xEventWake(loop)` 唤醒阻塞中的 `xEventWait`（替代原来的 `pthread_cond_signal`），使 timer 线程重新计算超时。
4. WHEN 跨线程取消 timer task（`xTimerCancel`）THEN 系统 SHALL 同样调用 `xEventWake(loop)` 唤醒 timer 线程。

#### 3.3 xTimerDetachEventLoop

##### 验收标准

1. WHEN 调用 `xTimerDetachEventLoop(timer)` THEN 系统 SHALL 解除 event loop 绑定，timer 后台线程恢复为 `pthread_cond_timedwait` 等待模式。
2. IF timer 未绑定 event loop THEN `xTimerDetachEventLoop` SHALL 返回 `xErrno_Unknown`。
3. IF timer 为 NULL THEN `xTimerDetachEventLoop` SHALL 返回 `xErrno_Unknown`。
4. WHEN detach 时 timer 线程正在 `xEventWait` 中阻塞 THEN 系统 SHALL 先通过 `xEventWake` 唤醒线程，再完成解绑。

#### 3.4 xTimerDestroy 兼容

##### 验收标准

1. WHEN 销毁一个已绑定 event loop 的 timer THEN `xTimerDestroy` SHALL 先自动 detach event loop，再执行原有的销毁流程。
2. WHEN 销毁一个未绑定 event loop 的 timer THEN `xTimerDestroy` SHALL 保持原有行为不变。

#### 3.5 方案 C 单元测试

##### 验收标准

1. WHEN 编写测试 THEN 测试 SHALL 覆盖以下场景：
   - `xTimerAttachEventLoop` 成功绑定
   - `xTimerAttachEventLoop` 重复绑定返回错误
   - `xTimerAttachEventLoop` NULL 参数返回错误
   - 绑定后 timer 到期回调正常触发
   - 绑定后 I/O 事件正常触发
   - timer 到期和 I/O 事件混合调度
   - 跨线程提交 timer task 能唤醒 `xEventWait`
   - 跨线程取消 timer task 能唤醒 `xEventWait`
   - `xTimerDetachEventLoop` 成功解绑后 timer 恢复原有行为
   - `xTimerDetachEventLoop` 未绑定时返回错误
   - `xTimerDestroy` 自动 detach event loop
   - 定时回调的延迟精度在合理范围内（误差 < 50ms）
   - NULL 参数不崩溃

---

### 需求 4：分支与 PR 管理

**用户故事：** 作为一名开发者，我希望三个方案分别在独立分支上实现并各自发 PR，以便对比评审。

#### 验收标准

1. WHEN 实现方案 A THEN 代码 SHALL 提交到分支 `codebuddy/event-timer-attach` 并创建 PR。
2. WHEN 实现方案 B THEN 代码 SHALL 提交到分支 `codebuddy/event-timer-builtin` 并创建 PR。
3. WHEN 实现方案 C THEN 代码 SHALL 提交到分支 `codebuddy/event-timer-driven` 并创建 PR。
4. WHEN 三个分支均基于 main 最新代码创建 THEN 三个方案 SHALL 互不依赖。

---

## 技术约束

1. 所有新增 API 必须遵循项目现有的 `XCAPI` / `XDEF_HANDLE` / `XDEF_ENUM` 风格。
2. 头文件使用 `#ifndef` include guard，命名遵循 `XBASE_XXX_H` 格式。
3. 测试使用 Google Test (C++)，文件命名为 `*_test.cpp`。
4. 三个后端（kqueue / epoll / poll）均需同步修改（方案 B）。
5. 现有的 `xEventLoop` 和 `xTimer` 的公共 API 保持向后兼容，现有测试不能被破坏。
