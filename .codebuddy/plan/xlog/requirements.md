# 需求文档：xlog — 异步日志模块

## 引言

xlog 是 xKit 项目中的一个**独立高级异步日志模块**，与 xhttp 平级，位于 `xlog/` 目录下。它依赖 xbase 提供的事件循环（`xEventLoop`）、MPSC 无锁队列（`xMpsc`）、定时器（`xEventLoopTimerAfter`）等基础设施，但**不修改 xbase 的任何代码**。

xlog 的核心目标是：在多线程环境下，让日志的格式化和入队操作在调用线程完成（低延迟），而实际的文件 I/O 操作由事件循环线程异步执行（高吞吐）。模块支持三种工作模式（Timer / Notify / Mixed），由用户在创建 logger 时指定，以适应不同的延迟和吞吐需求。

### 依赖关系

- `xbase/event.h` — 事件循环、定时器、fd 事件注册
- `xbase/mpsc.h` — 无锁 MPSC 队列
- `xbase/time.h` — 单调时钟 `xMonoMs()`
- `xbase/base.h` — `XCAPI`、`XDEF_HANDLE`、`XDEF_ENUM`、`XDEF_STRUCT`、`xContainerOf` 等基础宏
- `xbase/error.h` — `xErrno` 错误码
- `xbase/log.h` — 可选桥接（将 xbase 的同步日志重定向到 xlog）

### 不修改的模块

- `xbase/log.h` / `xbase/log.c` — 保持原样不动

---

## 需求

### 需求 1：Logger 生命周期管理

**用户故事：** 作为一名开发者，我希望能够创建和销毁 logger 实例，以便在应用程序中灵活管理日志资源。

#### 验收标准

1. WHEN 调用 `xLoggerCreate(conf)` 并传入有效配置 THEN 系统 SHALL 返回一个非 NULL 的 `xLogger` 句柄。
2. IF 配置中的 `loop` 为 NULL THEN 系统 SHALL 返回 NULL 表示创建失败。
3. WHEN 调用 `xLoggerCreate(conf)` 且配置中指定了文件路径 THEN 系统 SHALL 以 append 模式打开文件，并通过 `ftell` 获取已有文件大小作为 `written` 的初始值。
4. WHEN 调用 `xLoggerDestroy(logger)` THEN 系统 SHALL 同步 flush 队列中所有剩余日志条目，关闭文件句柄，取消定时器（如有），移除 pipe 的 fd 事件注册（如有），释放所有内存。
5. IF `xLoggerDestroy` 传入 NULL THEN 系统 SHALL 安全地什么都不做（no-op）。

---

### 需求 2：日志级别

**用户故事：** 作为一名开发者，我希望日志支持多个级别，以便在不同环境下过滤不同重要程度的日志。

#### 验收标准

1. 系统 SHALL 提供以下 5 个日志级别（严重程度递增）：`xLogLevel_Debug`、`xLogLevel_Info`、`xLogLevel_Warn`、`xLogLevel_Error`、`xLogLevel_Fatal`。
2. WHEN 日志条目的级别低于 logger 配置的最低级别 THEN 系统 SHALL 在调用线程直接丢弃该条目，不入队、不格式化。
3. 系统 SHALL 提供便捷宏 `XLOG_DEBUG(logger, fmt, ...)`、`XLOG_INFO(...)`、`XLOG_WARN(...)`、`XLOG_ERROR(...)`、`XLOG_FATAL(...)` 用于快速记录日志。
4. WHEN 使用 `XLOG_FATAL` 记录日志 THEN 系统 SHALL 走同步路径（直接格式化并写入文件/stderr），然后调用 `abort()`，确保崩溃前日志可见。

---

### 需求 3：三种工作模式

**用户故事：** 作为一名开发者，我希望在创建 logger 时选择工作模式，以便根据场景在延迟和吞吐之间做出权衡。

#### 验收标准

##### 3.1 Timer 模式 (`xLogMode_Timer`)

1. WHEN logger 以 Timer 模式创建 THEN 系统 SHALL 注册一个周期性定时器（间隔由配置指定，默认 50ms）。
2. WHEN 定时器触发 THEN 系统 SHALL 在事件循环线程上从 MPSC 队列中取出所有日志条目并批量写入文件。
3. 系统 SHALL 不创建任何额外的 pipe fd。
4. IF MPSC 队列为空 THEN 定时器回调 SHALL 跳过写入操作，仅重新注册下一次定时器。

##### 3.2 Notify 模式 (`xLogMode_Notify`)

1. WHEN logger 以 Notify 模式创建 THEN 系统 SHALL 创建一对 pipe fd，并将读端通过 `xEventAdd` 注册到事件循环上。
2. WHEN 调用线程写入一条日志 THEN 系统 SHALL 将条目推入 MPSC 队列后，向 pipe 写端写入 1 字节以唤醒事件循环。
3. WHEN pipe 读端可读 THEN 系统 SHALL 在事件循环线程上 drain pipe，然后从 MPSC 队列中取出所有日志条目并写入文件。
4. 系统 SHALL 不创建定时器。

##### 3.3 Mixed 模式 (`xLogMode_Mixed`)

1. WHEN logger 以 Mixed 模式创建 THEN 系统 SHALL 同时创建定时器和 pipe。
2. 正常路径：定时器周期性 flush 队列中的日志条目。
3. 紧急路径：WHEN 日志级别 >= `xLogLevel_Error` THEN 系统 SHALL 额外向 pipe 写入 1 字节以立即唤醒事件循环。
4. 低于 Error 级别的日志 SHALL 仅依赖定时器 flush，不触发 pipe 写入。

---

### 需求 4：日志格式化

**用户故事：** 作为一名开发者，我希望日志条目包含时间戳、级别、消息等信息，以便快速定位问题。

#### 验收标准

1. 系统 SHALL 在**调用线程**完成日志消息的 `vsnprintf` 格式化，而非在事件循环线程。
2. 系统 SHALL 在**调用线程**采集时间戳（使用 `xMonoMs()` 或 `gettimeofday`），确保时间准确反映日志产生时刻。
3. 每条日志的输出格式 SHALL 为：`YYYY-MM-DD HH:MM:SS.mmm <LEVEL> <message>\n`。
4. 格式化缓冲区大小 SHALL 默认为 512 字节（可通过编译宏 `XLOG_ENTRY_BUF_SIZE` 覆盖）。
5. IF 格式化后的消息超过缓冲区大小 THEN 系统 SHALL 截断消息（不崩溃）。

---

### 需求 5：日志文件轮转

**用户故事：** 作为一名开发者，我希望日志文件在达到一定大小后自动轮转，以便防止单个日志文件过大、磁盘被撑满。

#### 验收标准

##### 5.1 轮转触发

1. WHEN 当前日志文件的已写入字节数 `written >= max_size` THEN 系统 SHALL 触发轮转。
2. 轮转检查 SHALL 在事件循环线程的 flush 循环中，每写完一条日志后执行。
3. IF `max_size == 0` 或 `max_files <= 1` THEN 系统 SHALL 不执行轮转（单文件无限增长）。

##### 5.2 轮转流程

1. 系统 SHALL 关闭当前文件句柄。
2. 系统 SHALL 删除编号最大的旧文件（`path.{max_files-1}`），如果存在。
3. 系统 SHALL 从最旧到最新级联重命名：`path.{i-1}` → `path.{i}`（i 从 `max_files-1` 到 2）。
4. 系统 SHALL 将当前文件重命名为 `path.1`。
5. 系统 SHALL 以 append 模式重新打开 `path`，并将 `written` 重置为 0。

##### 5.3 命名规则

1. 轮转文件命名 SHALL 遵循 `<base_path>.1`、`<base_path>.2`、...、`<base_path>.{N}` 的规则，编号越小越新。

##### 5.4 `max_files` 语义

1. `max_files` SHALL 表示总共保留的文件数（包括当前正在写入的文件）。
2. `max_files = 0` 或 `max_files = 1` SHALL 表示不轮转。
3. `max_files = 5` SHALL 表示保留 `path` + `path.1` ~ `path.4`。

##### 5.5 重启恢复

1. WHEN logger 创建时文件已存在 THEN 系统 SHALL 通过 `fseek(SEEK_END)` + `ftell` 获取已有文件大小，作为 `written` 的初始值，避免重启后忽略已有内容。

---

### 需求 6：输出目标

**用户故事：** 作为一名开发者，我希望日志可以输出到文件或 stderr，以便在开发和生产环境中灵活选择。

#### 验收标准

1. IF 配置中指定了文件路径 (`path != NULL`) THEN 系统 SHALL 将日志写入该文件。
2. IF 配置中未指定文件路径 (`path == NULL`) THEN 系统 SHALL 将日志写入 stderr。
3. WHEN 输出到 stderr THEN 系统 SHALL 不执行文件轮转。

---

### 需求 7：线程安全

**用户故事：** 作为一名开发者，我希望可以从任意线程安全地写入日志，以便在多线程应用中无需额外加锁。

#### 验收标准

1. `xLoggerLog` 及所有便捷宏 SHALL 可从任意线程安全调用（多生产者）。
2. 文件 I/O（写入、轮转、flush）SHALL 仅在事件循环线程执行（单消费者）。
3. 系统 SHALL 使用 `xMpsc` 无锁队列实现生产者-消费者模型，生产者线程无需获取任何锁。

---

### 需求 8：同步 Flush

**用户故事：** 作为一名开发者，我希望能够手动触发同步 flush，以便在关键时刻确保日志已落盘。

#### 验收标准

1. WHEN 调用 `xLoggerFlush(logger)` THEN 系统 SHALL 同步等待事件循环线程将队列中所有当前日志条目写入文件并 `fflush`。
2. `xLoggerFlush` SHALL 可从任意线程调用。
3. `xLoggerDestroy` 内部 SHALL 隐式调用 flush 逻辑，确保销毁前所有日志已落盘。

---

### 需求 9：xbase/log 桥接（可选功能）

**用户故事：** 作为一名开发者，我希望能够将现有的 `xLog()` 调用重定向到 xlog 异步日志系统，以便统一日志输出而不修改已有代码。

#### 验收标准

1. 系统 SHALL 提供 `xLoggerEnter(logger)` 函数，调用后通过 `xLogSetCallback` 将当前线程的 `xLog()` 输出重定向到指定的 logger（进入该 logger 的上下文）。
2. 系统 SHALL 提供 `xLoggerLeave()` 函数，退出当前 logger 上下文，恢复 `xLog()` 的默认行为（输出到 stderr）。
3. 桥接 SHALL 利用 xbase/log 现有的 `xLogSetCallback` 机制，不修改 xbase 的任何代码。
4. IF 桥接的 `xLog()` 调用带有 `fatal=true` THEN 系统 SHALL 走同步路径写入日志后再 `abort()`。

---

### 需求 10：配置结构体

**用户故事：** 作为一名开发者，我希望通过一个配置结构体一次性指定 logger 的所有参数，以便创建过程简洁明了。

#### 验收标准

1. 系统 SHALL 提供 `xLoggerConf` 结构体，包含以下字段：
   - `loop` (`xEventLoop`) — 必选，事件循环句柄
   - `path` (`const char *`) — 可选，日志文件路径（NULL 表示输出到 stderr）
   - `mode` (`xLogMode`) — 工作模式，默认 `xLogMode_Timer`
   - `level` (`xLogLevel`) — 最低日志级别，默认 `xLogLevel_Info`
   - `max_size` (`size_t`) — 单个日志文件最大字节数，默认 0（不轮转）
   - `max_files` (`int`) — 保留的日志文件总数（含当前文件），默认 0（不轮转）
   - `flush_interval_ms` (`uint64_t`) — Timer/Mixed 模式的 flush 间隔，默认 50ms
2. IF `mode` 为 `xLogMode_Timer` 或 `xLogMode_Mixed` 且 `flush_interval_ms == 0` THEN 系统 SHALL 使用默认值 50ms。

---

### 需求 11：构建集成

**用户故事：** 作为一名开发者，我希望 xlog 模块能够无缝集成到现有的 CMake 构建系统中。

#### 验收标准

1. 系统 SHALL 在 `xlog/` 目录下提供独立的 `CMakeLists.txt`。
2. xlog 库 SHALL 链接 xbase 库。
3. 根目录 `CMakeLists.txt` SHALL 通过 `add_subdirectory(xlog)` 引入 xlog 模块。
4. IF `XK_BUILD_TESTS` 为 ON THEN 系统 SHALL 构建 xlog 的单元测试。

---

### 需求 12：文件组织

**用户故事：** 作为一名开发者，我希望 xlog 模块的文件组织清晰，遵循项目现有的风格规范。

#### 验收标准

1. 公共头文件 SHALL 为 `xlog/logger.h`，包含所有公共 API 声明。
2. 私有头文件 SHALL 为 `xlog/logger_private.h`，包含内部结构体定义。
3. 实现文件 SHALL 为 `xlog/logger.c`，包含所有实现逻辑。
4. 测试文件 SHALL 为 `xlog/logger_test.cpp`，使用 Google Test 框架。
5. 所有文件 SHALL 遵循 STYLE.md 中定义的命名规范、注释规范和格式化规范。
