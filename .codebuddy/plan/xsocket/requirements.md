# 需求文档：xSocket — 异步 Socket 抽象层

## 引言

在 xbase 现有的事件循环（`xEventLoop`）基础上，设计一个轻量级的异步 socket 抽象层 `xSocket`。该模块封装 POSIX socket 的创建、非阻塞设置、事件循环注册等样板代码，提供统一的回调驱动接口，使上层模块（如未来的 TCP server/client）能够以事件驱动方式进行网络 I/O。

### 设计原则

- **与 xEventLoop 深度集成**：socket 创建即绑定到事件循环，I/O 就绪通过回调通知
- **遵循 xKit 现有约定**：使用 `XDEF_HANDLE`、`XCAPI`、`xErrno` 等宏和类型
- **薄封装**：不做协议解析，只负责 fd 生命周期管理 + 事件循环注册
- **边缘触发兼容**：配合 xEventLoop 的 edge-triggered 语义

### 参数设计分析

用户提出的 `xSocketCreate(loop, family, type, protocol, callback, userp)` 参数基本完整，但经过分析需要补充以下内容：

1. **`loop`** — 必须，绑定的事件循环
2. **`family`** — 必须，地址族（AF_INET / AF_INET6 / AF_UNIX）
3. **`type`** — 必须，套接字类型（SOCK_STREAM / SOCK_DGRAM）
4. **`protocol`** — 必须，协议号（通常为 0）
5. **`callback`**（`xSocketFunc`）— 必须，I/O 就绪回调，签名为 `void (*)(xSocket, xEventMask, void *)`
6. **`userp`** — 必须，用户数据指针

**需要补充的参数：**

- **`mask`**（`xEventMask`）— 初始监听的事件掩码（读/写/读写）。不同场景需要不同的初始事件：listen socket 只关心读，connect 中的 socket 先关心写。如果不提供，调用者需要在创建后立即调用 `xEventMod` 修改，增加了使用复杂度。

> **关于 `mask` 的两层语义：** `xSocketCreate` 的 `mask` 参数是**注册掩码**，表示"我关心哪些事件"；而回调中的 `mask` 参数是**就绪掩码**，表示"当前触发的是哪些事件"。两者类型相同（`xEventMask`）但语义不同。回调中通过 `mask & xEvent_Read` / `mask & xEvent_Write` 即可区分当前需要处理的事件类型。

**需要引入的回调类型：**

- **`xSocketFunc`** — xSocket 专用的回调类型，替代直接使用 `xEventFunc`。

> **为什么不直接复用 `xEventFunc`？** `xEventFunc` 的签名是 `void (*)(int fd, xEventMask mask, void *arg)`，其中 `fd` 参数对于 `xSocket` 的使用者来说是冗余的：用户已经通过 `xSocket` handle 封装了 fd，回调里再暴露裸 fd 破坏了抽象层的封装。因此 `xSocket` 定义自己的回调类型 `xSocketFunc`，签名为 `void (*)(xSocket sock, xEventMask mask, void *arg)`，将 `fd` 替换为 `xSocket` handle。xSocket 内部注册 `xEventFunc` 到事件循环，在内部回调中完成 fd → xSocket 的转换后再调用用户的 `xSocketFunc`。

> 注：`nonblock` 和 `cloexec` 标志不需要作为参数，因为异步 socket **必须**是非阻塞的，`CLOEXEC` 也应该默认设置，这些是内部实现细节。

---

## 需求

### 需求 1：Socket 创建与事件循环绑定

**用户故事：** 作为一名网络模块开发者，我希望通过一个函数调用完成 socket 创建、非阻塞设置和事件循环注册，以便减少样板代码并避免遗漏关键步骤。

#### 验收标准

1. WHEN `xSocketCreate` is called with valid parameters THEN the system SHALL create a socket fd via `socket(family, type, protocol)`, set it to non-blocking mode (`O_NONBLOCK`) and close-on-exec (`FD_CLOEXEC`), register an internal `xEventFunc` trampoline with the event loop via `xEventAdd(loop, fd, mask, trampoline, internal_ctx)`, and return an opaque `xSocket` handle. The internal trampoline SHALL convert the raw fd callback into a `xSocketFunc` callback, passing the `xSocket` handle instead of the fd.

2. IF `loop` is NULL or `callback` is NULL THEN `xSocketCreate` SHALL return NULL without creating any socket.

3. IF the underlying `socket()` syscall fails THEN `xSocketCreate` SHALL return NULL.

4. IF `xEventAdd()` fails after socket creation THEN `xSocketCreate` SHALL close the fd and return NULL (no resource leak).

5. WHEN `xSocketCreate` succeeds THEN the returned `xSocket` handle SHALL provide access to the underlying fd via `xSocketFd()` for use with `bind()`, `listen()`, `connect()`, `send()`, `recv()` etc.

### 需求 2：Socket 销毁与资源清理

**用户故事：** 作为一名网络模块开发者，我希望销毁 socket 时自动从事件循环注销并关闭 fd，以便避免资源泄漏。

#### 验收标准

1. WHEN `xSocketDestroy` is called THEN the system SHALL remove the event source from the event loop via `xEventDel()`, close the underlying fd, and free the handle memory.

2. IF `xSocketDestroy` is called with NULL THEN the system SHALL do nothing (safe no-op).

3. WHEN `xSocketDestroy` completes THEN the handle SHALL be invalid and must not be used again.

### 需求 3：事件掩码修改

**用户故事：** 作为一名网络模块开发者，我希望在运行时修改 socket 监听的事件类型（读/写），以便实现状态机驱动的协议处理（如连接建立后从写切换到读）。

#### 验收标准

1. WHEN `xSocketSetMask` is called with a new event mask THEN the system SHALL update the event source via `xEventMod()`.

2. IF `xSocketSetMask` is called with an invalid socket handle THEN the system SHALL return `xErrno_InvalidArg`.

3. WHEN `xSocketSetMask` succeeds THEN the system SHALL return `xErrno_Ok` and subsequent event notifications SHALL reflect the new mask.

### 需求 4：辅助查询接口

**用户故事：** 作为一名网络模块开发者，我希望能查询 socket 的底层 fd 和当前事件掩码，以便在需要时直接调用 POSIX API 或进行调试。

#### 验收标准

1. WHEN `xSocketFd` is called THEN the system SHALL return the underlying file descriptor.

2. WHEN `xSocketMask` is called THEN the system SHALL return the current event mask.

3. IF either function is called with NULL THEN the system SHALL return -1 (for fd) or 0 (for mask).

### 需求 5：读写超时管理

**用户故事：** 作为一名网络模块开发者，我希望为 socket 设置读/写空闲超时，以便在对端无响应时及时发现并处理超时连接，而无需手动管理定时器。

#### 验收标准

1. WHEN `xSocketSetTimeout` is called with `read_timeout_ms > 0` THEN the system SHALL use the `xEventLoop` bound at creation time to register a read idle timer via `xEventLoopTimerAfter(loop, read_timeout_ms, ...)`. IF no read-ready event arrives within `read_timeout_ms` milliseconds THEN the timer SHALL fire and invoke the `xSocketFunc` callback with `mask` containing `xEvent_Timeout`.

2. WHEN `xSocketSetTimeout` is called with `write_timeout_ms > 0` THEN the system SHALL use the `xEventLoop` bound at creation time to register a write idle timer via `xEventLoopTimerAfter(loop, write_timeout_ms, ...)`. IF no write-ready event arrives within `write_timeout_ms` milliseconds THEN the timer SHALL fire and invoke the `xSocketFunc` callback with `mask` containing `xEvent_Timeout`.

3. WHEN a normal read-ready event arrives THEN the system SHALL reset (cancel and re-register) the read idle timer. WHEN a normal write-ready event arrives THEN the system SHALL reset the write idle timer. (idle timeout semantics)

4. IF `read_timeout_ms` is 0 THEN the system SHALL cancel any existing read timeout timer. IF `write_timeout_ms` is 0 THEN the system SHALL cancel any existing write timeout timer.

5. WHEN `xSocketSetTimeout` is called multiple times THEN the system SHALL replace the previous timeout settings.

6. WHEN `xSocketDestroy` is called THEN the system SHALL cancel all pending timeout timers before closing the socket.

7. `xEvent_Timeout` SHALL be defined as a new event mask bit (`1 << 2`) that does not conflict with existing `xEvent_Read` and `xEvent_Write` masks.

8. WHEN the timeout callback fires THEN the `xSocketFunc` callback SHALL receive the same `xSocket` handle and `userp` as normal I/O events, allowing unified event handling in a single callback function.

### 需求 6：API 签名设计

**用户故事：** 作为一名 xKit 用户，我希望 socket API 与现有的 xbase 风格一致（opaque handle、`XCAPI` 导出、`xErrno` 返回值），以便无缝融入现有代码。

#### 验收标准

1. `xSocket` SHALL be defined as an opaque handle via `XDEF_HANDLE(xSocket)`.

2. 所有公开 API SHALL 使用 `XCAPI(T)` 宏导出。

3. 完整的 API 签名 SHALL 为：

```c
/* Socket callback — receives xSocket handle instead of raw fd */
typedef void (*xSocketFunc)(xSocket sock, xEventMask mask, void *arg);

/* Create */
XCAPI(xSocket) xSocketCreate(xEventLoop loop,
                              int family, int type, int protocol,
                              xEventMask mask,
                              xSocketFunc callback, void *userp);

/* Destroy */
XCAPI(void) xSocketDestroy(xEventLoop loop, xSocket sock);

/* Modify event mask */
XCAPI(xErrno) xSocketSetMask(xEventLoop loop, xSocket sock, xEventMask mask);

/* Timeout */
XCAPI(xErrno) xSocketSetTimeout(xSocket sock,
                                 int read_timeout_ms, int write_timeout_ms);

/* Query */
XCAPI(int)        xSocketFd(xSocket sock);
XCAPI(xEventMask) xSocketMask(xSocket sock);
```

5. `xEvent_Timeout` SHALL be defined as `(1 << 2)`, not conflicting with `xEvent_Read` and `xEvent_Write`.

4. `xSocketDestroy` 和 `xSocketSetMask` SHALL 接受 `xEventLoop loop` 参数，与 `xEventDel` / `xEventMod` 的签名保持一致。

5. `xSocketSetTimeout` SHALL NOT 接受 `xEventLoop loop` 参数，因为 `xSocket` 在 `xSocketCreate` 时已绑定了事件循环，内部直接使用绑定的 `loop` 注册/取消定时器。这意味着 `xSocket` 内部结构需要保存 `loop` 的引用。

### 需求 7：头文件组织

**用户故事：** 作为一名 xKit 用户，我希望 socket.h 是一个纯头文件声明，实现放在 socket.c 中，以便保持模块化。

#### 验收标准

1. `xbase/socket.h` SHALL 只包含类型定义、函数声明和必要的 `#include`。

2. `xbase/socket.h` SHALL include `<xbase/base.h>`、`<xbase/error.h>` 和 `<xbase/event.h>`。

3. `xbase/socket.h` SHALL 使用 include guard `XBASE_SOCKET_H`。

4. 所有函数 SHALL 有 Doxygen 风格的文档注释，说明参数含义、返回值和错误条件。

### 需求 8：单元测试全覆盖

**用户故事：** 作为一名 xKit 开发者，我希望 xSocket 模块的所有功能都有完整的单元测试覆盖，以便在后续迭代中快速发现回归问题。

#### 验收标准

1. WHEN the test suite is executed THEN it SHALL cover **all** acceptance criteria defined in requirements 1–7, with at least one test case per acceptance criterion.

2. The test suite SHALL cover the following categories:

   **创建与销毁：**
   - 正常创建并验证 handle 非 NULL、fd 有效、非阻塞和 CLOEXEC 标志已设置
   - 传入 NULL loop 或 NULL callback 时返回 NULL
   - socket() 系统调用失败时返回 NULL（可通过无效 family 触发）
   - xEventAdd() 失败后无 fd 泄漏
   - 正常销毁后 fd 已关闭、事件已注销
   - 销毁 NULL handle 不崩溃（safe no-op）

   **事件掩码修改：**
   - 修改掩码后 xSocketMask() 返回新值
   - 传入无效 handle 返回 xErrno_InvalidArg

   **辅助查询接口：**
   - xSocketFd() 返回有效 fd
   - xSocketMask() 返回当前掩码
   - 传入 NULL 时 xSocketFd() 返回 -1，xSocketMask() 返回 0

   **读写超时管理：**
   - 设置读超时后，无读事件到达时回调收到 xEvent_Timeout
   - 设置写超时后，无写事件到达时回调收到 xEvent_Timeout
   - 正常 I/O 事件到达后定时器被重置（idle timeout 语义）
   - 传入 0 取消已有超时定时器
   - 多次调用 xSocketSetTimeout 替换之前的设置
   - xSocketDestroy 时自动取消所有待触发的超时定时器

   **回调机制：**
   - 回调收到的 xSocket handle 与创建时返回的一致
   - 回调收到的 userp 与创建时传入的一致
   - 回调中的 mask 正确反映就绪事件类型（读/写/超时）

3. WHEN any public API function has an error path THEN there SHALL be a dedicated test case exercising that error path.

4. All test cases SHALL be independent of each other and SHALL NOT rely on execution order.

### 需求 9：代码风格一致性

**用户故事：** 作为一名 xKit 开发者，我希望 xSocket 模块的代码风格与项目现有代码完全一致，以便保持代码库的统一性和可维护性。

#### 验收标准

1. **版权头：** 所有 `.h`、`.c`、`.cpp` 文件 SHALL 以 MIT 版权头块注释开头，格式与现有文件一致：
   ```c
   /*
    * Copyright 2025 The xKit Authors. All rights reserved.
    * Use of this source code is governed by a MIT license that can be
    * found in the LICENSE file.
    *
    * <filename> - <brief description>
    */
   ```

2. **头文件风格：**
   - SHALL 使用 `#ifndef XBASE_SOCKET_H` / `#define XBASE_SOCKET_H` include guard
   - SHALL 使用 `XDEF_HANDLE`、`XDEF_STRUCT`、`XDEF_ENUM` 宏定义类型
   - SHALL 使用 `XCAPI(T)` 宏导出所有公开函数
   - SHALL 使用 Doxygen 风格 `/** @brief ... @param ... @return ... */` 文档注释
   - SHALL 使用 `/* ── Section ──── */` 风格的分区注释组织代码

3. **实现文件风格：**
   - 内部结构体 SHALL 命名为 `struct xSocket_`（类型名 + 下划线后缀）
   - `static` 前向声明 SHALL 放在文件顶部
   - SHALL 使用 `calloc` 分配内存并显式类型转换
   - 错误处理 SHALL 采用逐步回退释放资源的模式（避免 goto）
   - SHALL 使用 `/* ── Section ──── */` 风格的分区注释

4. **测试文件风格：**
   - SHALL 使用 Google Test 框架，文件名为 `socket_test.cpp`
   - SHALL 使用 C++ 编写（`.cpp` 扩展名）
   - 测试用例 SHALL 使用 `TEST(GroupName, TestName)` 命名，GroupName 按功能分组
   - Helper 函数 SHALL 使用 `static` 声明，并用分区注释组织
   - 前置条件 SHALL 使用 `ASSERT_*` 宏，验证断言 SHALL 使用 `EXPECT_*` 宏

5. **命名规范：**
   - 公开 API SHALL 使用 `xSocket` 前缀（如 `xSocketCreate`、`xSocketDestroy`）
   - 枚举值 SHALL 使用 `xEvent_` 前缀（如 `xEvent_Timeout`）
   - 回调类型 SHALL 使用 `xSocketFunc` 命名风格
   - 内部函数 SHALL 使用 `static` 且采用 `snake_case` 命名
