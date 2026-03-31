# 需求文档

## 引言

在 `xbase/throw.h` 中设计一套**线程级（`__thread`）轻量异常/错误抛出机制**。核心思路是：每个线程可以通过 `xThrowSetCallback` 注册一个回调函数，当该线程调用 `xThrow(fmt, ...)` 时，格式化后的错误消息会被传递给该回调。这套机制为 xKit 提供了一种非侵入式的、可定制的错误上报通道，类似于 per-thread panic handler。

### 设计背景

- 项目使用 C（兼容 C++），API 风格遵循 `x` 前缀 + CamelCase 命名
- 已有 `xErrno` 错误码体系，`xThrow` 作为补充，用于不可恢复/需要立即上报的错误场景
- 需要线程安全，每个线程独立的回调互不干扰

---

## 需求

### 需求 1：注册线程级回调

**用户故事：** 作为一名库使用者，我希望能为当前线程设置一个错误回调函数，以便在该线程触发 `xThrow` 时收到格式化后的错误消息。

#### 验收标准

1. WHEN 用户调用 `xThrowSetCallback(cb)` THEN 系统 SHALL 将 `cb` 存储为当前线程的 `__thread` 级回调
2. WHEN 用户调用 `xThrowSetCallback(NULL)` THEN 系统 SHALL 清除当前线程的回调（恢复为无回调状态）
3. WHEN 线程 A 设置了回调 THEN 线程 B 的回调 SHALL 不受影响
4. WHEN 用户多次调用 `xThrowSetCallback` THEN 系统 SHALL 以最后一次设置的回调为准

### 需求 2：触发异常抛出

**用户故事：** 作为一名库使用者，我希望能通过 `xThrow(fmt, ...)` 触发当前线程的错误回调，以便将格式化的错误信息传递给回调处理。

#### 验收标准

1. WHEN 用户调用 `xThrow(fmt, ...)` AND 当前线程已设置回调 THEN 系统 SHALL 使用 `vsnprintf` 格式化消息并调用回调函数，将格式化后的字符串传入
2. WHEN 用户调用 `xThrow(fmt, ...)` AND 当前线程未设置回调 THEN 系统 SHALL 将格式化后的消息输出到 `stderr` 作为兜底行为
3. WHEN `fmt` 为 NULL THEN 系统 SHALL 安全处理，不崩溃（可忽略或使用默认消息如 `"(null)"`）

### 需求 3：回调函数签名设计

**用户故事：** 作为一名库使用者，我希望回调函数能接收到足够的上下文信息，以便灵活地处理错误（如记录日志、上报监控等）。

#### 验收标准

1. WHEN 回调被触发 THEN 回调 SHALL 至少接收格式化后的错误消息字符串（`const char *msg`）
2. WHEN 用户注册回调时 THEN 系统 SHALL 支持传入一个 `void *userdata` 上下文指针，在回调触发时一并传回
3. IF 回调签名为 `void (*xThrowCallback)(const char *msg, void *userdata)` THEN 系统 SHALL 在 `xThrowSetCallback` 中同时接收 callback 和 userdata 两个参数

### 需求 4：格式化缓冲区管理

**用户故事：** 作为一名库使用者，我希望 `xThrow` 的格式化过程是线程安全且高效的，以便在高并发场景下不会出现竞争或性能问题。

#### 验收标准

1. WHEN `xThrow` 格式化消息 THEN 系统 SHALL 使用线程局部的静态缓冲区（`__thread static char buf[...]`），避免堆分配
2. WHEN 格式化后的消息超过缓冲区大小 THEN 系统 SHALL 截断消息（`vsnprintf` 天然支持），不产生缓冲区溢出
3. IF 缓冲区大小需要可配置 THEN 系统 SHALL 提供一个编译期宏 `XTHROW_BUF_SIZE`（默认 512 字节），用户可在编译时覆盖

### 需求 5：API 风格一致性

**用户故事：** 作为一名库维护者，我希望 `xThrow` 模块的 API 风格与项目其他模块保持一致，以便代码库整体风格统一。

#### 验收标准

1. WHEN 声明公开 API THEN 系统 SHALL 使用 `XCAPI(T)` 宏包裹返回类型
2. WHEN 定义类型 THEN 系统 SHALL 使用 `XDEF_STRUCT` / `XDEF_ENUM` 等项目宏
3. WHEN 编写头文件 THEN 系统 SHALL 使用 `#ifndef XBASE_THROW_H` 头文件保护
4. WHEN 编写头文件 THEN 系统 SHALL 包含版权声明和模块说明注释

### 需求 6：与 xTask 集成考虑

**用户故事：** 作为一名库使用者，我希望在 `xTask` 的 worker 线程中也能正常使用 `xThrow`，以便任务函数内部的错误能被正确捕获。

#### 验收标准

1. WHEN 任务函数在 worker 线程中调用 `xThrow` AND 该 worker 线程已设置回调 THEN 系统 SHALL 正常触发该线程的回调
2. WHEN 任务函数在 worker 线程中调用 `xThrow` AND 该 worker 线程未设置回调 THEN 系统 SHALL 回退到 `stderr` 输出
3. IF 未来需要为 xTaskGroup 统一设置 throw 回调 THEN API 设计 SHALL 预留扩展空间（当前不实现，但不阻碍未来添加）
