# xsignal 需求文档

## 引言

xsignal 是 xKit/xbase 模块中的信号处理子系统，将 POSIX 信号集成到 `xEventLoop` 事件循环中。其核心目标是让用户能够以事件回调的方式处理 UNIX 信号（如 SIGINT、SIGTERM），而无需直接面对 async-signal-safe 的限制。

信号处理是 daemon 进程的刚需。传统的 `signal()` / `sigaction()` 处理函数运行在信号上下文中，能做的事情极其有限。xsignal 通过平台原生机制将信号转化为事件循环中的普通回调，用户回调在事件循环线程中执行，可以安全地调用任意函数。

### 设计原则

- **极简 API**：只暴露一个函数 `xEventLoopSignalWatch`，注册和取消都通过它完成
- **平台特化**：每个后端使用最优的原生机制，而非统一的 lowest-common-denominator 方案
- **就地集成**：信号逻辑直接写入各后端现有的 `event_kqueue.c` / `event_epoll.c` / `event_poll.c` 中，不单独创建后端文件
- **零新增文件**：API 声明放在 `event.h`，实现写入各后端 `event_*.c`，测试写入 `event_test.cpp`，不新增任何文件
- **风格一致**：遵循项目现有的 `XCAPI` / `XDEF_ENUM` / 条件编译 / 命名约定（`xEventLoop*` 前缀）

### 平台后端策略

| 平台 | 后端 | 机制 | 特点 |
|------|------|------|------|
| macOS / BSD | kqueue | `EVFILT_SIGNAL` | 零额外 fd，直接在 kqueue 上注册信号过滤器 |
| Linux | epoll | `signalfd` | 信号转化为可读 fd，注册到 epoll 作为普通事件源 |
| POSIX fallback | poll | self-pipe trick | 信号处理函数写 pipe，poll 监听 pipe 读端 |

---

## 需求

### 需求 1：公开 API — `xEventLoopSignalWatch`

**用户故事：** 作为一名使用 xKit 的 C 开发者，我希望通过一个函数就能注册和取消信号监听，以便用最少的代码实现优雅的信号处理。

#### API 签名

```c
typedef void (*xEventSignalFunc)(int signo, void *arg);

XCAPI(xErrno) xEventLoopSignalWatch(xEventLoop loop, int signo,
                                     xEventSignalFunc fn, void *arg);
```

- 注册：`xEventLoopSignalWatch(loop, SIGINT, on_sigint, ctx)`
- 取消：`xEventLoopSignalWatch(loop, SIGINT, NULL, NULL)` — 传 `NULL` 回调即取消，恢复 `SIG_DFL`
- 替换：对同一 `signo` 再次调用，新的 `fn` + `arg` 替换旧的

#### 验收标准

1. WHEN 用户调用 `xEventLoopSignalWatch(loop, signo, fn, arg)` 且 `fn` 不为 NULL THEN 系统 SHALL 注册对信号 `signo` 的监听，后续该信号触发时在事件循环线程中调用 `fn(signo, arg)`
2. WHEN 用户调用 `xEventLoopSignalWatch(loop, signo, NULL, NULL)` THEN 系统 SHALL 取消对信号 `signo` 的监听，并恢复该信号的默认处理行为（`SIG_DFL`）
3. WHEN 用户对同一个 `signo` 重复调用 `xEventLoopSignalWatch` 且 `fn` 不为 NULL THEN 系统 SHALL 替换之前的回调为新的 `fn` 和 `arg`
4. WHEN `loop` 为 NULL 或 `signo` 无效（如 SIGKILL、SIGSTOP、负数、超出范围）THEN 系统 SHALL 返回 `xErrno_InvalidArg`
5. WHEN 注册成功 THEN 系统 SHALL 返回 `xErrno_Ok`
6. WHEN 底层系统调用失败（如 signalfd、kevent 失败）THEN 系统 SHALL 返回 `xErrno_SysError` 且不改变当前状态

### 需求 2：回调类型定义 — `xEventSignalFunc`

**用户故事：** 作为一名使用 xKit 的 C 开发者，我希望信号回调的签名清晰明确，以便我知道回调被调用时能获取哪些信息。

#### 验收标准

1. WHEN 信号回调被定义 THEN 系统 SHALL 在 `event.h` 中提供类型 `typedef void (*xEventSignalFunc)(int signo, void *arg)`，与现有 `xEventFunc` / `xEventTimerFunc` 并列
2. WHEN 信号触发且回调被调用 THEN 系统 SHALL 传入触发的信号编号 `signo` 和注册时提供的 `arg` 指针

### 需求 3：kqueue 后端实现（macOS / BSD）

**用户故事：** 作为一名在 macOS 上开发的 C 开发者，我希望信号处理利用 kqueue 的 `EVFILT_SIGNAL` 原生能力，以便获得最优性能且不引入额外的文件描述符。

#### 验收标准

1. WHEN 编译环境定义了 `XK_HAS_KQUEUE` THEN 系统 SHALL 使用 kqueue 的 `EVFILT_SIGNAL` 机制实现信号监听
2. WHEN 注册信号监听 THEN 系统 SHALL 调用 `signal(signo, SIG_IGN)` 阻止默认处理，并通过 `EV_SET` + `kevent` 注册 `EVFILT_SIGNAL` 过滤器
3. WHEN `xEventWait` 中 `kevent` 返回的事件 `filter == EVFILT_SIGNAL` THEN 系统 SHALL 识别为信号事件并调用对应的用户回调
4. WHEN 取消信号监听 THEN 系统 SHALL 通过 `EV_DELETE` 移除 `EVFILT_SIGNAL` 过滤器，并调用 `signal(signo, SIG_DFL)` 恢复默认处理
5. IF kqueue 后端被选中 THEN 系统 SHALL 不创建任何额外的文件描述符用于信号处理

### 需求 4：epoll 后端实现（Linux）

**用户故事：** 作为一名在 Linux 上开发的 C 开发者，我希望信号处理利用 `signalfd` 机制，以便信号作为普通可读 fd 集成到 epoll 中，无需修改现有事件分发逻辑。

#### 验收标准

1. WHEN 编译环境定义了 `XK_HAS_EPOLL` THEN 系统 SHALL 使用 `signalfd` 机制实现信号监听
2. WHEN 注册信号监听 THEN 系统 SHALL 通过 `sigprocmask` 阻塞目标信号，创建 `signalfd`，并将其注册到 epoll 作为可读事件源
3. WHEN signalfd 变为可读 THEN 系统 SHALL 读取 `struct signalfd_siginfo`，提取信号编号，并调用对应的用户回调
4. WHEN 取消信号监听 THEN 系统 SHALL 从 epoll 移除 signalfd，关闭 signalfd，并通过 `sigprocmask` 解除信号阻塞、恢复 `SIG_DFL`
5. WHEN 对已有 signalfd 的信号重复注册 THEN 系统 SHALL 仅替换回调和 arg，不重新创建 signalfd

### 需求 5：poll 后端实现（POSIX fallback）

**用户故事：** 作为一名在非 kqueue/epoll 平台上开发的 C 开发者，我希望信号处理通过 self-pipe trick 实现，以便在任何 POSIX 系统上都能工作。

#### 验收标准

1. WHEN 编译环境既未定义 `XK_HAS_KQUEUE` 也未定义 `XK_HAS_EPOLL` THEN 系统 SHALL 使用 self-pipe trick 实现信号监听
2. WHEN 注册信号监听 THEN 系统 SHALL 创建一对 pipe，将写端保存在全局状态中，通过 `sigaction` 安装信号处理函数（处理函数仅向 pipe 写入信号编号），并将读端注册到事件循环
3. WHEN pipe 读端可读 THEN 系统 SHALL 读取信号编号并调用对应的用户回调
4. WHEN 取消信号监听 THEN 系统 SHALL 从事件循环移除 pipe 读端，关闭 pipe 两端，并通过 `sigaction` 恢复 `SIG_DFL`
5. IF 信号在短时间内多次触发 THEN 系统 SHALL 至少调用一次回调（信号合并是 POSIX 信号的固有行为）

### 需求 6：事件循环集成

**用户故事：** 作为一名使用 xKit 的 C 开发者，我希望信号回调在事件循环线程中执行，以便我可以在回调中安全地调用任意函数（包括 `xEventLoopStop`）。

#### 验收标准

1. WHEN 信号触发 THEN 系统 SHALL 确保用户回调在 `xEventWait` 的调用线程中执行，而非在信号处理上下文中
2. WHEN 用户在信号回调中调用 `xEventLoopStop(loop)` THEN 系统 SHALL 正常停止事件循环，不产生死锁或未定义行为
3. WHEN 事件循环通过 `xEventLoopDestroy` 销毁 THEN 系统 SHALL 不要求用户手动取消所有信号监听（但也不自动取消，信号状态由用户管理）

### 需求 7：文件组织

**用户故事：** 作为一名 xKit 的维护者，我希望信号功能完全集成到现有文件中，不新增任何文件，以便代码库保持简洁。

#### 验收标准

1. WHEN 信号 API 被声明 THEN 系统 SHALL 将 `xEventSignalFunc` 类型和 `xEventLoopSignalWatch` 函数声明添加到现有的 `event.h` 中，与 `xEventLoopTimerAfter` 等函数并列
2. WHEN 各后端的信号处理逻辑被实现 THEN 系统 SHALL 直接写入现有的后端文件中：
   - `event_kqueue.c` — 增加 kqueue `EVFILT_SIGNAL` 相关逻辑
   - `event_epoll.c` — 增加 `signalfd` 相关逻辑
   - `event_poll.c` — 增加 self-pipe trick 相关逻辑
3. WHEN 信号功能的内部数据结构被定义 THEN 系统 SHALL 将信号监视数组添加到 `event_base.h` 中各后端的 `xEventLoop*_` 结构体中
4. WHEN 信号功能的单元测试被编写 THEN 系统 SHALL 将测试用例添加到现有的 `event_test.cpp` 中

### 需求 8：各后端事件分发集成

**用户故事：** 作为一名 xKit 的维护者，我希望各后端的事件分发逻辑能正确识别和处理信号事件，以便信号回调能被正确分发。

#### 验收标准

1. WHEN kqueue 后端的 `xEventWait` 收到 `filter == EVFILT_SIGNAL` 的事件 THEN 系统 SHALL 通过 `events[i].ident` 获取信号编号，查找对应的回调并执行
2. WHEN epoll 后端的 `xEventWait` 收到 signalfd 的可读事件 THEN 系统 SHALL 读取 `struct signalfd_siginfo`，提取信号编号，查找对应的回调并执行
3. WHEN poll 后端的 `xEventWait` 收到 signal pipe 读端的可读事件 THEN 系统 SHALL 读取信号编号，查找对应的回调并执行
4. WHEN 修改各后端文件 THEN 系统 SHALL 将改动控制在最小范围内，在现有事件分发逻辑中增加信号事件的处理分支

### 需求 9：单元测试

**用户故事：** 作为一名 xKit 的维护者，我希望信号功能有完整的单元测试覆盖，以便确保各后端实现的正确性。

#### 验收标准

1. WHEN 测试基本注册功能 THEN 测试 SHALL 验证 `xEventLoopSignalWatch` 注册后，通过 `raise(signo)` 或 `kill(getpid(), signo)` 发送信号能触发回调
2. WHEN 测试取消功能 THEN 测试 SHALL 验证 `xEventLoopSignalWatch(loop, signo, NULL, NULL)` 后信号不再触发回调
3. WHEN 测试替换功能 THEN 测试 SHALL 验证对同一信号重复注册会替换回调
4. WHEN 测试参数校验 THEN 测试 SHALL 验证 NULL loop、无效 signo 返回 `xErrno_InvalidArg`
5. WHEN 测试多信号 THEN 测试 SHALL 验证同时监听多个不同信号各自独立工作
6. WHEN 测试与事件循环集成 THEN 测试 SHALL 验证在信号回调中调用 `xEventLoopStop` 能正常停止循环
7. WHEN 测试使用信号 THEN 测试 SHALL 使用 `SIGUSR1` / `SIGUSR2` 等用户自定义信号，避免干扰测试框架

### 需求 10：限制与约束

**用户故事：** 作为一名使用 xKit 的 C 开发者，我希望清楚了解信号功能的限制，以便正确使用。

#### 验收标准

1. IF 用户尝试监听 `SIGKILL` 或 `SIGSTOP` THEN 系统 SHALL 返回 `xErrno_InvalidArg`（这两个信号无法被捕获）
2. IF 同一信号在短时间内多次触发 THEN 系统 SHALL 至少调用一次回调（信号合并是 POSIX 固有行为，不保证每次信号都产生独立回调）
3. IF 用户从非事件循环线程调用 `xEventLoopSignalWatch` THEN 行为 SHALL 是未定义的（与 `xEventAdd` 等函数一致，事件循环操作限制在 loop 线程）
4. IF 用户在多个事件循环上对同一信号注册监听 THEN 行为 SHALL 是未定义的（一个信号只应绑定到一个事件循环）
