# xbase Windows 移植状态

## 已完成

| 文件 | 改动内容 |
|---|---|
| `atomic.h` | 内存序常量改为独立定义；MSVC 路径用 `Interlocked*`；修复 `_xInterlockedCas` 失败时未更新 `*e` 的 bug |
| `memory.c` | `__ATOMIC_SEQ_CST` → `xAtomicSeqCst` |
| `time.c` | `clock_gettime` → `QueryPerformanceCounter` / `GetSystemTimeAsFileTime`（Win7 兼容） |
| `task.c` | pthread → `thread_private.h` 抽象层；`stdatomic.h` → `xAtomic*`；原子字段改为 `long` |
| `thread_private.h` | 新增内部平台抽象头文件：mutex/cond/thread/once，Win7 兼容（`CRITICAL_SECTION`/`CONDITION_VARIABLE`/`INIT_ONCE`） |
| `timer.c` | pthread → `thread_private.h`；`pthread_cond_timedwait` → `SleepConditionVariableCS`（相对超时，更简单） |
| `note.h` | Windows 路径：`SwitchToThread()` 替代 `sched_yield()`，`_mm_pause()` 替代 `__builtin_ia32_pause()`，Level 3 用 yield 循环 |
| `log.c` | `__thread` → `__declspec(thread)`（Win）/ `__thread`（POSIX） |
| `io.h` / `io.c` | 引入 `xSsize`/`xOff`/`xIovec` 平台抽象类型；Windows 下自定义 `xIovec` 替代 `struct iovec`；`errno` 检查加 `_WIN32` 守卫 |
| `event_private.h` | wake pipe → `CreateEvent`（手动重置）；`pthread_mutex_t` → `xMutex`；`usleep` → `Sleep(0)`；`fcntl`/`unistd.h` 加 `_WIN32` 守卫 |
| `event_timer.c` | `pthread_mutex_lock/unlock` → `xMutexLock/Unlock` |

## 完全不兼容（需重写）

| 文件 | 问题 |
|---|---|
| `event_epoll.c` | Linux epoll 专属，需 Windows 事件后端替代 |
| `event_kqueue.c` | macOS/BSD kqueue 专属 |
| `event_poll.c` | POSIX `poll()`/`pipe()`/`fcntl()`，Windows 上不可用 |
| `command.c` / `command.h` | 深度依赖 `fork()`/`exec*()`/`waitpid()`/`forkpty()` 等 POSIX 进程 API，需用 `CreateProcess` 完全重写 |

## 部分不兼容（需平台适配）

| 文件 | 不兼容 API | Windows 替代方案 |
|---|---|---|
| `socket.c` | POSIX socket: `socket()`, `fcntl()`, `close()`, `SOCK_CLOEXEC` | Winsock2: `WSASocket()`, `ioctlsocket()`, `closesocket()` |
| `backtrace.c` | `libunwind`/`execinfo.h`, `signal(SIGBUS)` | `CaptureStackBackTrace` / `SetUnhandledExceptionFilter` |
| `mpsc.c` | 依赖 `atomic.h` | 已通过 `atomic.h` 改造间接兼容 |

## 已兼容 Windows

- `slab.c` — 已有 `#if defined(_WIN32)` 分支使用 `VirtualAlloc/VirtualFree`
- `compat.h` — 提供 `memmem` polyfill，Windows 上自动启用
- 纯算法模块（无平台依赖）：`array`, `base58`, `base64`, `bitmap`, `heap`, `hex`, `map`, `string`, `list`, `error`, `flag`, `speed_tracker`

## CMakeLists.txt

当前无 Windows 平台分支。三个事件后端（epoll/kqueue/poll）在 Windows 上全部不会编译，事件循环完全不可用。

## 移植优先级

1. ~~**P0** `atomic.h`~~ ✅
2. ~~**P0** `time.c`~~ ✅
3. ~~**P0** `task.c` + `thread_private.h`~~ ✅
4. ~~**P1** `timer.c`~~ ✅
5. ~~**P1** `note.h`~~ ✅
6. ~~**P2** `io.h`/`io.c`~~ ✅
7. ~~**P2** `log.c`~~ ✅
8. ~~**P0** `event_private.h` + `event_timer.c`~~ ✅
9. **P0** `event_iocp.c`（新建）— 实现 Windows 事件循环后端
10. **P1** `socket.c` — POSIX socket → Winsock2
11. **P1** `command.c` — `fork/exec` → `CreateProcess`，工作量大
12. **P2** `backtrace.c` — Windows 栈回溯
13. **P2** `CMakeLists.txt` — 添加 Windows 平台分支
