# 实施计划

## 方案 A — 轻量集成（分支 `codebuddy/event-timer-attach`）

- [ ] 1. 从 main 创建分支 `codebuddy/event-timer-attach`
  - `git checkout main && git pull && git checkout -b codebuddy/event-timer-attach`
  - _需求：4_

- [ ] 2. 在 `timer.h` / `timer.c` 中实现 `xTimerNextDeadline` API
  - 在 `timer.h` 中声明 `XCAPI(uint64_t) xTimerNextDeadline(xTimer t)`
  - 在 `timer.c` 中实现：加锁读取 heap 堆顶的 deadline，堆空返回 `UINT64_MAX`，NULL 参数返回 `UINT64_MAX`
  - _需求：1.1_

- [ ] 3. 修改 `event.h` 和 `event_base.h`，扩展 `xEventLoopCreate` 签名
  - 将 `xEventLoopCreate(void)` 改为 `xEventLoopCreate(xTimer timer)`，`timer.h` 需要前向声明或 include
  - 在 `struct xEventLoop_` 中新增 `xTimer timer` 和 `int stopped` 字段
  - 在 `event.h` 中声明 `xEventLoopRun` 和 `xEventLoopStop`
  - _需求：1.2, 1.3_

- [ ] 4. 修改三个后端实现（`event_kqueue.c` / `event_epoll.c` / `event_poll.c`）适配新签名
  - `xEventLoopCreate` 接收 `xTimer` 参数并存入 `loop->timer`
  - 校验 timer 非 NULL 时必须是 poll 模式（`xTimerPoll` 返回 0 即可判断，或新增内部辅助函数）
  - 保持 `timer == NULL` 时与原有行为完全一致
  - _需求：1.2_

- [ ] 5. 实现 `xEventLoopRun` / `xEventLoopStop`
  - 可在任一后端文件或新建 `event_run.c` 中实现（因为逻辑与后端无关，仅调用公共 API）
  - `xEventLoopRun`：循环调用 `xTimerNextDeadline` 计算 timeout → `xEventWait` → `xTimerPoll`，检查 `stopped` 标志
  - `xEventLoopStop`：设置 `stopped = 1`，调用 `xEventWake`
  - NULL 参数安全处理
  - _需求：1.3_

- [ ] 6. 修复现有 `event_test.cpp` 中 `xEventLoopCreate()` 调用以适配新签名
  - 所有现有调用改为 `xEventLoopCreate(NULL)`，确保原有 24 个测试全部通过
  - _需求：1.2（向后兼容）_

- [ ] 7. 编写方案 A 集成测试
  - 新建 `event_timer_attach_test.cpp`（或追加到 `event_test.cpp`）
  - 覆盖：`xTimerNextDeadline` 空堆 / 正常值、创建时传入 poll/push/NULL timer、`xEventLoopRun` 混合调度 I/O + timer、`xEventLoopStop` 终止、延迟精度、NULL 安全
  - _需求：1.4_

- [ ] 8. 提交代码并创建 PR
  - 编译通过 + 全部测试通过后提交并推送，使用 `gh pr create`
  - _需求：4_

---

## 方案 B — 深度集成（分支 `codebuddy/event-timer-builtin`）

- [ ] 9. 从 main 创建分支 `codebuddy/event-timer-builtin`
  - `git checkout main && git checkout -b codebuddy/event-timer-builtin`
  - _需求：4_

- [ ] 10. 修改 `event.h`，新增 timer 相关 API 声明
  - 新增 `XDEF_HANDLE(xEventTimer)` 句柄类型
  - 声明 `xEventLoopTimerAfter`、`xEventLoopTimerAt`、`xEventLoopTimerCancel`
  - 声明 `xEventLoopRun`、`xEventLoopStop`
  - 新增 `typedef void (*xEventTimerFunc)(void *arg)` 回调类型
  - _需求：2.2, 2.3, 2.4, 2.6_

- [ ] 11. 修改 `event_base.h`，在 `struct xEventLoop_` 中嵌入 heap 和锁
  - 新增 `#include <xbase/heap.h>` 和 `#include <pthread.h>`
  - 在 `struct xEventLoop_` 中新增 `xHeap timer_heap`、`pthread_mutex_t timer_mu`、`int stopped` 字段
  - 定义内部 `struct xEventTimer_` 结构体（deadline、fn、arg、heap_idx、fired）
  - 新增 heap 比较函数和 set_idx 回调（static inline）
  - _需求：2.1, 2.7_

- [ ] 12. 修改三个后端实现，集成 timer 到 `xEventLoopCreate` / `Destroy` / `Wait`
  - `xEventLoopCreate`：初始化 `timer_heap` 和 `timer_mu`
  - `xEventLoopDestroy`：释放 heap 中所有未到期 timer（不执行回调），销毁 heap 和 mutex
  - `xEventWait`：在调用系统 API 前用 `min(timeout, heap_top - now)` 调整超时；返回后 pop + fire 所有到期 timer
  - _需求：2.1, 2.5_

- [ ] 13. 实现 `xEventLoopTimerAfter` / `xEventLoopTimerAt` / `xEventLoopTimerCancel`
  - 可在新建 `event_timer.c` 中实现（与后端无关）
  - 加锁操作 heap，提交后调用 `xEventWake` 唤醒阻塞中的 `xEventWait`
  - Cancel 从 heap 中移除并释放，已执行的返回错误
  - NULL 参数安全处理
  - _需求：2.2, 2.3, 2.4, 2.7_

- [ ] 14. 实现 `xEventLoopRun` / `xEventLoopStop`
  - `xEventLoopRun`：循环调用 `xEventWait(loop, -1)` 直到 `stopped` 标志
  - `xEventLoopStop`：设置 `stopped = 1`，调用 `xEventWake`
  - _需求：2.6_

- [ ] 15. 编写方案 B 单元测试
  - 新建 `event_timer_builtin_test.cpp`
  - 覆盖：TimerAfter 基本 / delay=0、TimerAt 正常 / 过期、Cancel 成功 / 已执行、多 timer 顺序、混合 I/O + timer、Run + Stop、跨线程提交、销毁丢弃未到期 timer、NULL 安全、延迟精度
  - _需求：2.8_

- [ ] 16. 提交代码并创建 PR
  - 编译通过 + 全部测试通过后提交并推送
  - _需求：4_

---

## 方案 C — Timer 驱动（分支 `codebuddy/event-timer-driven`）

- [ ] 17. 从 main 创建分支 `codebuddy/event-timer-driven`
  - `git checkout main && git checkout -b codebuddy/event-timer-driven`
  - _需求：4_

- [ ] 18. 修改 `timer.h`，新增 Attach / Detach API 声明
  - 声明 `XCAPI(xErrno) xTimerAttachEventLoop(xTimer t, xEventLoop loop)`
  - 声明 `XCAPI(xErrno) xTimerDetachEventLoop(xTimer t)`
  - 新增 `#include <xbase/event.h>`（或前向声明 `xEventLoop`）
  - _需求：3.1, 3.3_

- [ ] 19. 修改 `timer.c` 中 `struct xTimer_`，新增 event loop 字段
  - 新增 `xEventLoop loop` 字段（NULL 表示未绑定）
  - _需求：3.1_

- [ ] 20. 修改 `timer.c` 中 `timer_thread`，支持双模式等待
  - 当 `t->loop == NULL` 时保持原有 `pthread_cond_timedwait` 行为
  - 当 `t->loop != NULL` 时：解锁 → 调用 `xEventWait(t->loop, timeout)` → 加锁 → fire 到期 timer
  - timeout 由 heap 堆顶 deadline 计算，heap 空时传 -1（无限等待）
  - _需求：3.2_

- [ ] 21. 实现 `xTimerAttachEventLoop` / `xTimerDetachEventLoop`
  - Attach：加锁设置 `t->loop`，signal cond 唤醒线程切换到新模式；重复绑定或 NULL 参数返回错误
  - Detach：加锁清除 `t->loop`，先 `xEventWake` 唤醒阻塞中的 `xEventWait`，再 signal cond；未绑定或 NULL 参数返回错误
  - _需求：3.1, 3.3_

- [ ] 22. 修改 `timer.c` 中 `submit` 和 `xTimerCancel`，绑定时用 `xEventWake` 替代 `pthread_cond_signal`
  - `submit`：push heap 后，若 `t->loop != NULL` 则调用 `xEventWake(t->loop)`，否则 `pthread_cond_signal`
  - `xTimerCancel`：remove heap 后，若 `t->loop != NULL` 则调用 `xEventWake(t->loop)`
  - _需求：3.2_

- [ ] 23. 修改 `xTimerDestroy`，销毁前自动 detach
  - 若 `t->loop != NULL`，先调用 `xTimerDetachEventLoop` 再执行原有销毁流程
  - _需求：3.4_

- [ ] 24. 编写方案 C 单元测试
  - 新建 `timer_event_driven_test.cpp`
  - 覆盖：Attach 成功 / 重复绑定 / NULL 参数、绑定后 timer 触发、绑定后 I/O 触发、混合调度、跨线程提交唤醒、跨线程取消唤醒、Detach 恢复原有行为 / 未绑定返回错误、Destroy 自动 detach、延迟精度、NULL 安全
  - _需求：3.5_

- [ ] 25. 提交代码并创建 PR
  - 编译通过 + 全部测试通过后提交并推送
  - _需求：4_
