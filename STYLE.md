# xKit Coding Style Guide

本文档总结了 xKit 项目的编码规范和命名风格，方便新人快速上手。

项目使用 `.clang-format` 进行自动格式化（基于 LLVM 风格），以下规范侧重于
clang-format **无法覆盖**的约定。

---

## 1. 语言与标准

| 项目 | 规范 |
| ------ | ------ |
| 实现语言 | C (C99+) |
| 测试语言 | C++ (Google Test) |
| 注释语言 | English |
| 构建系统 | CMake ≥ 3.14 |

---

## 2. 文件组织

### 2.1 目录结构

```plain
modules/
  xbase/        # 核心原语 — 事件循环、定时器、任务、异步 socket、内存、无锁数据结构
  xbuf/         # 缓冲区原语 — 线性、环形、链式 I/O 缓冲区
  xnet/         # 网络原语 — URL 解析、异步 DNS、TCP、TLS 传输层
  xhttp/        # 异步 HTTP — libcurl 客户端、HTTP/1.1 & HTTP/2 服务器、WebSocket
  xlog/         # 异步日志 — MPSC 队列、定时/管道刷写、日志轮转
  xcrypto/      # 密码学原语 — SHA-1（OpenSSL / mbedTLS / builtin）
  xp2p/         # P2P 连接 — ICE agent、STUN/TURN、SDP、NAT 穿透、DTLS、DataChannel
  xfer/         # P2P 文件传输 — 零配置发送/接收、分块、SHA-1 校验、断点续传
examples/       # 示例程序
bench/          # 端到端基准测试（HTTP server vs Go 等）
cmake/          # CMake 辅助模块（Find*.cmake、Functions.cmake）
scripts/        # 构建 & 测试脚本
docs/           # 文档站点源文件
```

### 2.2 文件命名

| 类型 | 命名规则 | 示例 |
| ------ | ---------- | ------ |
| 公共头文件 | `<module>.h` | `event.h`, `timer.h`, `time.h` |
| 私有头文件 | `<module>_private.h` | `transport_private.h`, `event_private.h` |
| 实现文件 | `<module>.c` 或 `<module>_<variant>.c` | `event_kqueue.c`, `event_epoll.c` |
| 测试文件 | `<module>_test.cpp` | `heap_test.cpp`, `timer_test.cpp` |

**头文件可见性层级：**

| 层级 | 后缀 | 可见范围 | 典型内容 |
| ------ | ------ | ------ | ------ |
| public | `<module>.h` | 外部用户 + 项目内所有模块 | 类型定义、枚举、opaque handle |
| private | `<module>_private.h` | 项目内部（同模块 + 跨模块均可） | 内部结构体字段、工厂/初始化函数等不对外发布的接口 |

`private` 头文件 include 对应的 `public` 头文件，形成单向依赖链：`private → public`。

### 2.3 文件头注释

每个文件必须以版权声明 + 简要描述开头：

```c
/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * <filename> - <Brief one-line description>
 *
 * <Optional multi-line detailed description.>
 */
```

### 2.4 头文件保护

使用 `#ifndef` / `#define` / `#endif` 风格，命名为 `<MODULE>_<FILE>_H`：

```c
#ifndef XBASE_EVENT_H
#define XBASE_EVENT_H
// ...
#endif /* XBASE_EVENT_H */
```

---

## 3. 命名规范

### 3.1 总览

| 元素 | 风格 | 示例 |
| ------ | ------ | ------ |
| 公共函数 | `x` + PascalCase | `xHeapPush`, `xMonoMs`, `xEventLoopCreate` |
| 公共类型 | `x` + PascalCase | `xHeap`, `xTimer`, `xEventLoop` |
| 公共枚举值 | `x` + PascalCase + `_` + PascalCase | `xErrno_Ok`, `xEvent_Read` |
| 公共回调类型 | `x` + PascalCase + `Func` | `xEventFunc`, `xTimerFunc`, `xSseEventFunc` |
| 公共结构体 | `x` + PascalCase | `xHttpResponse`, `xSseEvent`, `xTaskGroupConf` |
| 公共宏 | `x` + PascalCase 或 `X` + UPPER_CASE | `xContainerOf`, `XCAPI`, `XDEF_STRUCT` |
| 内部结构体 | `x` + PascalCase + `_` (trailing) | `struct xEventSource_`, `struct xEventTimer_` |
| 内部/static 函数 | snake_case | `submit_timer`, `sources_add`, `loop_drain_wake` |
| 局部变量 | snake_case | `delay_ms`, `abs_ms`, `wake_rfd` |
| 结构体字段 | snake_case | `status_code`, `body_len`, `heap_idx` |
| 编译定义 | `XK_` + UPPER_CASE | `XK_HAS_KQUEUE`, `XK_HAS_EPOLL` |
| CMake 选项 | `XK_` + UPPER_CASE | `XK_BUILD_TESTS` |

### 3.2 命名原则

- **公共 API 一律以 `x` 前缀开头**，避免与用户代码冲突。
- **模块名嵌入函数名**：`xEventLoopCreate`、`xHttpClientGet`、`xTimerSubmitAfter`。
- **缩写优先简洁**：优先使用业界通用的简短缩写而非冗长的全称。
  - `xMonoMs` (monotonic) 而非 `xMonotonicMs`
  - `xWallMs` (wall clock) 而非 `xRealtimeMs`
- **回调类型以 `Func` 结尾**：`xEventFunc`、`xTimerFunc`、`xSseDoneFunc`。
- **配置结构体以 `Conf` 结尾**：`xTaskGroupConf`、`xHttpRequestConf`。
- **opaque handle 用 `XDEF_HANDLE`**：对外暴露为 `typedef void *xFoo`。

---

## 4. API 设计约定

### 4.1 导出宏

所有公共函数使用 `XCAPI(T)` 宏声明，自动处理 C/C++ 链接：

```c
XCAPI(xHeap) xHeapCreate(xHeapCmpFunc cmp, xHeapSetIdxFunc setidx, size_t cap);
XCAPI(void)  xHeapDestroy(xHeap h);
```

### 4.2 类型定义宏

| 宏 | 用途 | 展开 |
| ---- | ------ | ------ |
| `XDEF_STRUCT(T)` | 定义公共结构体 | `typedef struct T T; struct T` |
| `XDEF_ENUM(T)` | 定义枚举（底层为 `int`） | `typedef int T; enum` |
| `XDEF_HANDLE(T)` | 定义 opaque 指针 | `typedef void *T` |

### 4.3 错误处理

- 返回 `xErrno` 枚举值表示成功/失败。
- 创建函数返回指针/handle，失败返回 `NULL`。
- 错误码定义在 `<xbase/error.h>` 中，可通过 `xstrerror()` 获取描述。

### 4.4 Create / Destroy 配对

资源管理遵循对称的 Create/Destroy 模式：

```c
xTimer t = xTimerCreate(group);
// ... use t ...
xTimerDestroy(t);
```

### 4.5 Doxygen 注释

公共 API 使用 `/** ... */` 风格的 Doxygen 注释，包含 `@brief`、`@param`、`@return`：

```c
/**
 * @brief Push an element onto the heap.
 * @param h    The heap.
 * @param elem The element to insert.
 * @return xErrno_Ok on success.
 */
XCAPI(xErrno) xHeapPush(xHeap h, void *elem);
```

---

## 5. 格式化（clang-format 要点）

完整配置见 `.clang-format`，关键设定：

| 设定 | 值 |
| ------ | ---- |
| 行宽 | 80 列 |
| 缩进 | 2 空格，不使用 Tab |
| 指针对齐 | 靠右 (`int *p`) |
| 大括号 | Attach（K&R 风格） |
| 对齐 | 连续赋值、声明、宏自动对齐 |
| switch/case | case 不缩进 |
| 短函数 | 空函数体可单行 |
| 短 if | 无 else 时可单行 |

---

## 6. 内部实现约定

### 6.1 static 函数

- 文件内部函数使用 `static`，命名为 **snake_case**。
- 用 `/* ── Section ── */` 分隔符组织代码段落。

### 6.2 内部结构体

- 以 `_` 结尾：`struct xEventSource_`、`struct xEventTimer_`。
- 定义在 `*_private.h` 中，仅供同模块的 `.c` 文件 include。

### 6.3 inline 辅助函数

- 定义在 `*_private.h` 中的 `static inline` 函数，命名为 snake_case。
- 用于多个后端共享的通用逻辑（如 `loop_drain_wake`、`sources_add`）。

### 6.4 goto fail 错误处理模式

当一个函数包含**多步初始化**且失败时需要**统一释放已分配的资源**，应使用
`goto fail` 模式将清理逻辑收拢到函数末尾的 `fail:` 标签下，避免在每个错误
分支中重复编写相同的清理代码。

**适用场景：**

- 函数中有 2 个及以上需要清理的资源（fd、malloc、fopen 等）。
- 多个错误分支共享相同的清理逻辑。

**不适用场景：**

- 只有单一资源需要清理，直接 `return` 即可。
- 销毁/析构函数（每步都必须执行，不存在"失败跳转"语义）。
- 不同错误分支的清理行为完全不同。

**示例：**

```c
static int logger_make_pipe(int fds[2]) {
  if (pipe(fds) != 0) return -1;

  for (int i = 0; i < 2; i++) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    if (flags < 0) goto fail;
    if (fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
  }
  return 0;

fail:
  close(fds[0]);
  close(fds[1]);
  return -1;
}
```

**要点：**

- 标签统一命名为 `fail`，放在函数末尾、正常 `return` 之后。
- `fail:` 块中使用条件判断保护每个资源（如 `if (ptr) free(ptr)`），
  因为跳转可能发生在资源分配之前。
- 函数开头将资源变量初始化为安全的零值（`NULL`、`-1` 等），确保
  `fail:` 块中的条件判断正确。

---

## 7. 测试约定

- 使用 **Google Test** 框架，文件后缀 `_test.cpp`。
- 通过 `extern "C" { #include <xbase/xxx.h> }` 引入 C 头文件。
- 测试类命名为 PascalCase：`HeapTest`、`TimerTest`。
- 测试用例命名为 PascalCase：`PushSingleAndPop`、`MinProperty`。
- 用 `/* ========== Section ========== */` 注释分隔测试组。

---

## 8. 头文件 include 顺序

1. 对应的公共头文件（如 `time.c` → `#include <xbase/time.h>`）
2. 标准库头文件（`<stdint.h>`, `<stdlib.h>`, ...）
3. 系统头文件（`<pthread.h>`, `<unistd.h>`, ...）
4. 项目内部头文件（`"event_private.h"`）

使用尖括号 `<xbase/xxx.h>` 引用公共头文件，双引号 `"xxx.h"` 引用同目录私有头文件。

---

## 9. Commit 规范

遵循 [Conventional Commits](https://www.conventionalcommits.org/) 规范。

### 9.1 格式

```plain
<type>(<scope>): <subject>
```

- **type**：必选，表示提交类型。
- **scope**：可选，表示影响的模块，使用小写。
- **subject**：必选，简要描述变更，使用小写开头、祈使语气、不加句号。

### 9.2 Type 列表

| Type | 说明 |
| -------- | ---- |
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档变更 |
| `chore` | 构建、CI、杂项（不影响源码逻辑） |
| `ci` | CI 配置变更 |
| `refactor` | 重构（不新增功能、不修复 Bug） |
| `test` | 添加或修改测试 |
| `style` | 代码格式调整（不影响逻辑） |

### 9.3 Scope 列表

使用项目模块名作为 scope，常见值：

- `xbase` — 核心原语（事件循环、定时器、任务、内存等）
- `xbuf` — 缓冲区
- `xnet` — 网络（URL、DNS、TCP、TLS）
- `xhttp` — HTTP 客户端 & 服务器、WebSocket
- `xlog` — 异步日志
- `xcrypto` — 密码学
- `xp2p` — P2P 连接（ICE、STUN/TURN、DTLS、DataChannel）
- `xfer` — P2P 文件传输

scope 也可省略，表示跨模块或项目级变更。

### 9.4 示例

```plain
feat(xhttp): add SSE support
fix(task): add missing mutex lock before pthread_cond_wait in xTaskWait
docs: update README with centered logo and cleanup
ci: add GitHub Actions workflow for build and test
feat(error): add fine-grained error codes, replace xErrno_Unknown across modules
chore: remove .codebuddy from .gitignore
```

---

## 10. 分支名规范

### 10.1 格式

```plain
<author>/<short-description>
```

- **author**：开发者用户名，小写。
- **short-description**：简短描述，全小写，单词间用短横线 `-` 分隔。

### 10.2 命名规则

- 分支名必须以 `<author>/` 开头。
- 全部使用小写字母、数字和短横线 `-`。
- 描述部分应简洁明了，体现分支目的（功能、修复、模块名等）。
- 主分支为 `main`，不使用 `master`。
- PR 临时分支可使用 `pr<number>` 格式（如 `pr24`）。

### 10.3 示例

```plain
codebuddy/add-ci-workflow
codebuddy/fix-xappend
codebuddy/implement-task-module
codebuddy/improve-error-codes
codebuddy/xcurl
```

---

## 11. 版本号规范

版本号遵循 [Semantic Versioning 2.0](https://semver.org/)，直接在根 `CMakeLists.txt` 的 `project()` 中声明：

```cmake
project(xKit VERSION 0.0.1 LANGUAGES C CXX)

# Pre-release channel (alpha / beta / rc / "")
set(XK_VERSION_CHANNEL "alpha")
set(XK_VERSION_BUILDNO 1)
```

CMake 会自动设置 `PROJECT_VERSION`、`PROJECT_VERSION_MAJOR`、`PROJECT_VERSION_MINOR`、`PROJECT_VERSION_PATCH` 等变量。预发布通道和构建号通过额外的 `set()` 变量管理，最终拼接为完整版本字符串 `XK_VERSION`。

### 11.1 格式

```plain
MAJOR.MINOR.PATCH[-CHANNEL.BUILDNO]
```

| 字段 | 说明 | 示例 |
| -------- | ---- | ---- |
| `MAJOR` | 不兼容的 API 变更 | `1` |
| `MINOR` | 向后兼容的新功能 | `2` |
| `PATCH` | 向后兼容的 Bug 修复 | `3` |
| `CHANNEL` | 预发布通道（可选） | `alpha`, `beta`, `rc` |
| `BUILDNO` | 预发布构建号（可选） | `1`, `2` |

### 11.2 示例

```plain
0.0.1-alpha.1
1.0.0
2.0.0-beta.2
1.3.0-rc.1
```

---

## 12. CMake 规范

### 12.1 编译选项

所有编译选项通过 `option` 注册：

```cmake
option(XK_BUILD_TESTS "Build xKit tests" ON)
option(XK_BUILD_BENCHMARKS "Build xKit benchmarks" ON)
option(XK_BUILD_EXAMPLES "Build xKit example programs" OFF)
option(XK_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(XK_DEBUG_LEVEL "Debug logging level (0-3)" 0)
```

### 12.2 平台检测

使用 CMake 的 `check_include_file` 进行平台特性检测，编译定义统一为 `XK_HAS_<FEATURE>`：

```cmake
check_include_file(sys/event.h HAS_SYS_EVENT_H)   # → XK_HAS_KQUEUE
check_include_file(sys/epoll.h HAS_SYS_EPOLL_H)   # → XK_HAS_EPOLL
```

### 12.3 模块 CMakeLists

每个模块目录包含独立的 `CMakeLists.txt`，负责：

- 收集源文件（`file(GLOB_RECURSE ...)`）
- 构建共享库（`add_library(... SHARED ...)`）
- 条件编译测试（`if(XK_BUILD_TESTS) ... endif()`）

---

## 13. CI 规范

CI 配置位于 `.github/workflows/ci.yml`，所有 PR 和 `main` 分支推送自动触发。

### 13.1 构建矩阵

| 平台 | 编译器 |
| ---- | ---- |
| macOS (latest) | Clang |
| Ubuntu (latest) | GCC |

### 13.2 编译警告策略

- C 代码：`-Wall -Wextra -Werror`（警告即错误）
- C++ 测试：`-Wall -Wextra`（警告不阻断，因第三方测试框架可能产生警告）

### 13.3 检查项

- **构建**：`cmake --build` 必须成功。
- **测试**：`ctest --output-on-failure` 所有测试必须通过。
- **分支名 lint**：PR 分支名必须匹配 `<author>/` 前缀规则。

### 13.4 并发控制

同一 PR / 分支的新推送会自动取消正在运行的旧 CI，避免资源浪费。
