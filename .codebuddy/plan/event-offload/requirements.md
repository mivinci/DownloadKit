# 需求文档

## 引言

xKit 的事件循环（`xEventLoop`）当前在 `xEventWait` 中同步执行 I/O 回调和定时器回调。如果用户注册的回调函数执行耗时操作（如数据库查询、文件 I/O、CPU 密集计算），将阻塞整个事件循环，导致后续事件无法及时处理。

本功能为事件循环新增 **异步 offload** 能力：允许用户将耗时工作提交到 `xTaskGroup` 线程池执行，工作完成后通过已有的 wake pipe 机制将完成回调安全地投递回事件循环线程执行，保证回调与 I/O 事件串行化，用户无需加锁。

### 设计约束

- 复用已有的 `xTaskGroup` 线程池，不引入新的线程管理机制
- 复用已有的 wake pipe 唤醒机制，不增加额外的 fd
- 复用已有的 `xMpsc` lock-free 队列作为完成队列，不引入 mutex
- 完成回调（done callback）在事件循环线程上执行，与 I/O 回调串行化
- 不使用该功能时零开销（done queue 始终为空）
- 向后兼容，不改变现有 API 的行为

## 需求

### 需求 1：异步工作提交 API

**用户故事：** 作为一名使用 xKit 事件循环的开发者，我希望能够将耗时工作提交到线程池异步执行，以便事件循环不被阻塞。

#### 验收标准

1. WHEN 用户调用 `xEventLoopSubmit(loop, group, work_fn, done_fn, arg)` THEN 系统 SHALL 将 `work_fn` 提交到指定的 `xTaskGroup` 线程池执行
2. IF `group` 参数为 NULL THEN 系统 SHALL 使用 `xTaskGroupGlobal()` 返回的全局线程池
3. WHEN `work_fn` 在工作线程上执行完毕 THEN 系统 SHALL 将 `done_fn` 排入事件循环的完成队列，并通过 `xEventWake` 唤醒事件循环
4. WHEN 事件循环在 `xEventWait` 中被唤醒 THEN 系统 SHALL 依次执行完成队列中所有待处理的 `done_fn` 回调
5. IF `done_fn` 为 NULL THEN 系统 SHALL 支持 fire-and-forget 模式，工作完成后不执行任何回调
6. IF `loop` 或 `work_fn` 为 NULL THEN 系统 SHALL 返回 `xErrno_Unknown`
7. WHEN `xEventLoopSubmit` 被调用 THEN 系统 SHALL 返回 `xErrno_Ok` 表示提交成功，或返回错误码表示失败

### 需求 2：完成队列（Done Queue）内部机制

**用户故事：** 作为一名 xKit 维护者，我希望事件循环内部有一个线程安全的完成队列，以便工作线程能安全地将完成回调投递回事件循环线程。

#### 设计决策

完成队列复用项目已有的 `xMpsc`（Multi-Producer Single-Consumer）lock-free 队列，而非使用 mutex + 链表。理由：

- **场景完美匹配**：多个工作线程并发入队（multi-producer），仅事件循环线程出队（single-consumer）
- **零锁开销**：`xMpscPush` / `xMpscPop` 基于原子操作，无需 mutex
- **项目内已有先例**：`xTimer_` 的 poll-mode 队列使用了相同的 `xMpsc` 模式
- **侵入式设计**：工作项结构体内嵌 `xMpsc` 节点，无额外堆分配

#### 验收标准

1. WHEN 工作线程将完成项入队 THEN 系统 SHALL 使用 `xMpscPush` 进行 lock-free 入队
2. WHEN 事件循环线程 dispatch 完成队列 THEN 系统 SHALL 循环调用 `xMpscPop` 直到返回 NULL，依次处理所有待处理项
3. WHEN 完成队列中有多个待处理项 THEN 系统 SHALL 按 FIFO 顺序依次执行 `done_fn`
4. WHEN 事件循环被销毁 THEN 系统 SHALL 循环调用 `xMpscPop` 清理所有未处理的工作项，释放内存，不执行其 `done_fn`

### 需求 3：跨后端一致性

**用户故事：** 作为一名 xKit 用户，我希望异步 offload 功能在所有事件循环后端（kqueue / epoll / poll）上行为一致，以便我的代码可以跨平台运行。

#### 验收标准

1. WHEN 使用 kqueue 后端 THEN 系统 SHALL 在 `xEventWait` 的 drain wake pipe 之后 dispatch 完成队列
2. WHEN 使用 epoll 后端 THEN 系统 SHALL 在 `xEventWait` 的 drain wake pipe 之后 dispatch 完成队列
3. WHEN 使用 poll 后端 THEN 系统 SHALL 在 `xEventWait` 的 drain wake pipe 之后 dispatch 完成队列
4. WHEN `xEventLoopCreate` 被调用 THEN 系统 SHALL 初始化完成队列的 `xMpsc` head/tail 指针为 NULL
5. WHEN `xEventLoopDestroy` 被调用 THEN 系统 SHALL 循环 `xMpscPop` 释放所有残留工作项

### 需求 4：线程安全

**用户故事：** 作为一名 xKit 用户，我希望能从任意线程（包括 I/O 回调、定时器回调、外部线程）安全地调用 `xEventLoopSubmit`，以便灵活地使用异步 offload。

#### 验收标准

1. WHEN `xEventLoopSubmit` 从事件循环线程调用 THEN 系统 SHALL 正确提交工作
2. WHEN `xEventLoopSubmit` 从外部线程调用 THEN 系统 SHALL 正确提交工作
3. WHEN 多个线程并发调用 `xEventLoopSubmit` THEN 系统 SHALL 保证所有工作项都被正确提交和执行
4. WHEN `done_fn` 在事件循环线程上执行 THEN 系统 SHALL 保证 `done_fn` 与 I/O 回调、定时器回调串行执行，用户无需加锁

### 需求 5：单元测试

**用户故事：** 作为一名 xKit 维护者，我希望异步 offload 功能有充分的单元测试覆盖，以便确保功能正确性和回归安全。

#### 验收标准

1. WHEN 提交一个异步工作 THEN 测试 SHALL 验证 `work_fn` 在工作线程上被执行
2. WHEN 异步工作完成 THEN 测试 SHALL 验证 `done_fn` 在事件循环线程上被执行
3. WHEN `done_fn` 为 NULL THEN 测试 SHALL 验证 fire-and-forget 模式不会崩溃
4. WHEN 传入无效参数（NULL loop 或 NULL work_fn）THEN 测试 SHALL 验证返回错误码
5. WHEN 多个异步工作并发提交 THEN 测试 SHALL 验证所有工作项都被正确执行和回调
6. WHEN `work_fn` 返回结果指针 THEN 测试 SHALL 验证 `done_fn` 能正确接收该结果
