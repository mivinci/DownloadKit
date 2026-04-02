# 实施计划：xSocket — 异步 Socket 抽象层

- [ ] 1. 定义 `xbase/socket.h` 头文件
   - 添加 MIT 版权头、include guard (`XBASE_SOCKET_H`)
   - `#include <xbase/base.h>`、`<xbase/error.h>`、`<xbase/event.h>`
   - 使用 `XDEF_HANDLE(xSocket)` 定义 opaque handle
   - 定义 `xEvent_Timeout` 掩码 `(1 << 2)`
   - 定义 `xSocketFunc` 回调类型：`void (*)(xSocket sock, xEventMask mask, void *arg)`
   - 声明所有公开 API（`xSocketCreate`、`xSocketDestroy`、`xSocketSetMask`、`xSocketSetTimeout`、`xSocketFd`、`xSocketMask`），每个函数附带 Doxygen 文档注释
   - 使用 `/* ── Section ──── */` 分区注释组织代码
   - _需求：6、7、9_

- [ ] 2. 实现 `xbase/socket.c` 内部结构与基础设施
   - 添加 MIT 版权头
   - 定义 `struct xSocket_` 内部结构体，包含字段：`fd`、`loop`、`mask`、`callback`、`userp`、`read_timer_id`、`write_timer_id`、`read_timeout_ms`、`write_timeout_ms`
   - 实现 `static` 的 trampoline 回调函数，将 `xEventFunc(fd, mask, arg)` 转换为 `xSocketFunc(sock, mask, arg)` 调用
   - 实现 `static` 的超时回调函数（读/写各一个），触发时调用用户回调并传入 `xEvent_Timeout` 掩码
   - 将所有 `static` 前向声明放在文件顶部
   - _需求：1.1、5.1、5.2、9.3_

- [ ] 3. 实现 `xSocketCreate` 和 `xSocketDestroy`
   - `xSocketCreate`：参数校验（NULL loop / NULL callback 返回 NULL）→ `calloc` 分配 `xSocket_` → `socket()` 创建 fd → 设置 `O_NONBLOCK` + `FD_CLOEXEC` → `xEventAdd()` 注册 trampoline → 失败时逐步回退释放资源 → 保存 `loop` 引用到内部结构 → 返回 handle
   - `xSocketDestroy`：NULL 安全检查 → 取消所有超时定时器 → `xEventDel()` 注销事件 → `close(fd)` → `free` 释放内存
   - _需求：1.1、1.2、1.3、1.4、1.5、2.1、2.2、2.3、5.6_

- [ ] 4. 实现 `xSocketSetMask` 和辅助查询接口
   - `xSocketSetMask`：参数校验 → 调用 `xEventMod()` 更新事件掩码 → 更新内部 `mask` 字段 → 返回 `xErrno_Ok` 或 `xErrno_InvalidArg`
   - `xSocketFd`：NULL 返回 -1，否则返回 `sock->fd`
   - `xSocketMask`：NULL 返回 0，否则返回 `sock->mask`
   - _需求：3.1、3.2、3.3、4.1、4.2、4.3_

- [ ] 5. 实现 `xSocketSetTimeout` 超时管理
   - 读超时：`read_timeout_ms > 0` 时通过 `xEventLoopTimerAfter()` 注册读空闲定时器；`= 0` 时取消已有读定时器
   - 写超时：`write_timeout_ms > 0` 时注册写空闲定时器；`= 0` 时取消已有写定时器
   - 在 trampoline 回调中，当正常读就绪事件到达时重置读定时器，写就绪事件到达时重置写定时器（idle timeout 语义）
   - 多次调用时替换之前的设置
   - 超时触发时通过 `xSocketFunc` 回调传入 `xEvent_Timeout` 掩码、相同的 `xSocket` handle 和 `userp`
   - _需求：5.1、5.2、5.3、5.4、5.5、5.7、5.8_

- [ ] 6. 编写创建与销毁相关的单元测试（`xbase/socket_test.cpp`）
   - 添加 MIT 版权头，`#include <gtest/gtest.h>` 和 `xbase/socket.h`
   - `TEST(SocketCreate, Success)` — 正常创建，验证 handle 非 NULL、fd 有效、`O_NONBLOCK` 和 `FD_CLOEXEC` 标志已设置
   - `TEST(SocketCreate, NullLoop)` — 传入 NULL loop 返回 NULL
   - `TEST(SocketCreate, NullCallback)` — 传入 NULL callback 返回 NULL
   - `TEST(SocketCreate, InvalidFamily)` — 无效 family 导致 `socket()` 失败，返回 NULL
   - `TEST(SocketDestroy, Normal)` — 正常销毁后 fd 已关闭
   - `TEST(SocketDestroy, Null)` — 销毁 NULL 不崩溃
   - _需求：8（创建与销毁类别）_

- [ ] 7. 编写事件掩码与查询接口的单元测试
   - `TEST(SocketMask, SetAndGet)` — 修改掩码后 `xSocketMask()` 返回新值
   - `TEST(SocketMask, InvalidHandle)` — 传入无效 handle 返回 `xErrno_InvalidArg`
   - `TEST(SocketQuery, Fd)` — `xSocketFd()` 返回有效 fd
   - `TEST(SocketQuery, Mask)` — `xSocketMask()` 返回当前掩码
   - `TEST(SocketQuery, NullFd)` — 传入 NULL 时返回 -1
   - `TEST(SocketQuery, NullMask)` — 传入 NULL 时返回 0
   - _需求：8（事件掩码修改 + 辅助查询接口类别）_

- [ ] 8. 编写超时管理的单元测试
   - `TEST(SocketTimeout, ReadTimeout)` — 设置读超时，无读事件到达时回调收到 `xEvent_Timeout`
   - `TEST(SocketTimeout, WriteTimeout)` — 设置写超时，无写事件到达时回调收到 `xEvent_Timeout`
   - `TEST(SocketTimeout, IdleReset)` — 正常 I/O 事件到达后定时器被重置
   - `TEST(SocketTimeout, CancelWithZero)` — 传入 0 取消已有超时定时器
   - `TEST(SocketTimeout, ReplaceTimeout)` — 多次调用替换之前的设置
   - `TEST(SocketTimeout, DestroyCancel)` — `xSocketDestroy` 时自动取消所有待触发的超时定时器
   - _需求：8（读写超时管理类别）_

- [ ] 9. 编写回调机制的单元测试
   - `TEST(SocketCallback, HandleMatch)` — 回调收到的 `xSocket` handle 与创建时返回的一致
   - `TEST(SocketCallback, UserpMatch)` — 回调收到的 `userp` 与创建时传入的一致
   - `TEST(SocketCallback, MaskReflectsEvent)` — 回调中的 mask 正确反映就绪事件类型（读/写/超时）
   - 使用 `socketpair()` 或 `pipe()` 构造可控的 I/O 就绪场景
   - _需求：8（回调机制类别）_
