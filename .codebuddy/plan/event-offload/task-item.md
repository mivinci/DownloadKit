# 实施计划

- [ ] 1. 定义工作项结构体并扩展 `xEventLoop_` 基础结构
   - 在 `event_base.h` 中新增 `#include <xbase/mpsc.h>` 和 `#include <xbase/task.h>`
   - 定义 `struct xEventWork_` 结构体，包含字段：`xMpsc mpsc`（侵入式队列节点）、`xTaskFunc work_fn`、`void (*done_fn)(void *arg, void *result)`、`void *arg`、`void *result`、`xEventLoop loop`（回指事件循环的指针）
   - 在 `struct xEventLoop_` 中新增两个字段：`xMpsc *done_head` 和 `xMpsc *done_tail`，初始值为 NULL
   - _需求：2.1, 2.4_

- [ ] 2. 新增 `loop_dispatch_done` 内联辅助函数
   - 在 `event_base.h` 中实现 `static inline void loop_dispatch_done(struct xEventLoop_ *loop)`
   - 循环调用 `xMpscPop(&loop->done_head, &loop->done_tail)` 直到返回 NULL
   - 对每个弹出的节点，通过 `container_of` 或结构体首字段偏移获取 `xEventWork_`，若 `done_fn` 非 NULL 则调用 `done_fn(arg, result)`，然后 `free` 工作项
   - _需求：2.2, 2.3_

- [ ] 3. 在 `event_base.h` 中新增 `loop_cleanup_done` 辅助函数
   - 实现 `static inline void loop_cleanup_done(struct xEventLoop_ *loop)`
   - 循环调用 `xMpscPop` 清理所有残留工作项，仅 `free` 不执行 `done_fn`
   - 供各后端的 `xEventLoopDestroy` 调用
   - _需求：2.4, 3.5_

- [ ] 4. 在 `event.h` 中声明公共 API `xEventLoopSubmit`
   - 新增 `typedef void (*xEventDoneFunc)(void *arg, void *result)` 回调类型定义
   - 声明 `XCAPI(xErrno) xEventLoopSubmit(xEventLoop loop, xTaskGroup group, xTaskFunc work_fn, xEventDoneFunc done_fn, void *arg)`
   - 编写完整的 Doxygen 注释，说明参数语义（`group` 为 NULL 时使用全局线程池、`done_fn` 为 NULL 时为 fire-and-forget 模式）
   - _需求：1.1, 1.2, 1.5, 1.7_

- [ ] 5. 实现 `xEventLoopSubmit` 函数
   - 新建 `event_offload.c` 源文件（GLOB 自动收录，无需改 CMakeLists.txt）
   - 参数校验：`loop` 或 `work_fn` 为 NULL 时返回 `xErrno_Unknown`
   - `group` 为 NULL 时调用 `xTaskGroupGlobal()` 获取全局线程池
   - 分配 `xEventWork_` 并填充字段，将 `loop` 指针存入工作项
   - 定义内部 wrapper 函数 `offload_worker`，签名为 `void *(void *)`：执行 `work->work_fn(work->arg)` 并将返回值存入 `work->result`，然后调用 `xMpscPush(&loop->done_head, &loop->done_tail, &work->mpsc)` 入队，最后调用 `xEventWake(loop)` 唤醒事件循环，返回 NULL
   - 调用 `xTaskSubmit(group, offload_worker, work)` 提交到线程池；若返回 NULL 则 `free(work)` 并返回错误码
   - 提交成功返回 `xErrno_Ok`
   - _需求：1.1, 1.2, 1.3, 1.5, 1.6, 1.7, 4.1, 4.2, 4.3_

- [ ] 6. 在三个后端的 `xEventWait` 中集成完成队列 dispatch
   - **kqueue** (`event_kqueue.c`)：在 `loop_drain_wake` 之后、I/O 回调 dispatch 之前（或 wake pipe `continue` 之后），调用 `loop_dispatch_done(&loop->base)`
   - **epoll** (`event_epoll.c`)：同上位置调用 `loop_dispatch_done(&loop->base)`
   - **poll** (`event_poll.c`)：在 `loop_drain_wake` 之后调用 `loop_dispatch_done(&loop->base)`
   - 注意：三个后端的 wake pipe 检测位置略有不同，需分别在各自的 drain 之后紧跟 dispatch
   - _需求：3.1, 3.2, 3.3, 1.4, 4.4_

- [ ] 7. 在三个后端的 `xEventLoopCreate` 中初始化完成队列
   - 在 kqueue / epoll / poll 三个后端的 `xEventLoopCreate` 中，在 `sources_init` 之后添加 `loop->base.done_head = NULL; loop->base.done_tail = NULL;`
   - _需求：3.4_

- [ ] 8. 在三个后端的 `xEventLoopDestroy` 中清理完成队列
   - 在 kqueue / epoll / poll 三个后端的 `xEventLoopDestroy` 中，在释放 timer heap 之后、`free(loop)` 之前，调用 `loop_cleanup_done(&loop->base)`
   - _需求：2.4, 3.5_

- [ ] 9. 编写单元测试 `event_offload_test.cpp`
   - 创建 `xbase/event_offload_test.cpp`（GLOB 自动收录）
   - **基本 offload 测试**：提交一个工作，验证 `work_fn` 被执行、`done_fn` 在事件循环线程被回调，且能接收到 `work_fn` 的返回值 → _需求：5.1, 5.2, 5.6_
   - **fire-and-forget 测试**：`done_fn` 传 NULL，验证不崩溃 → _需求：5.3_
   - **参数校验测试**：NULL loop / NULL work_fn 返回 `xErrno_Unknown` → _需求：5.4_
   - **并发提交测试**：多线程并发调用 `xEventLoopSubmit`，验证所有工作项都被正确执行和回调 → _需求：5.5_
   - **group 为 NULL 测试**：验证使用全局线程池正常工作 → _需求：1.2_
   - _需求：5.1, 5.2, 5.3, 5.4, 5.5, 5.6_
