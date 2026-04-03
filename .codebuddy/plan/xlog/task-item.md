# 实施计划

- [ ] 1. 创建 `xlog/` 目录结构和 CMake 构建集成
   - 创建 `xlog/` 目录，添加 `xlog/CMakeLists.txt`，参照 xhttp 的模式：`GLOB_RECURSE` 收集源文件，`add_library(xlog SHARED ...)`，`target_link_libraries(xlog PUBLIC xbase)`
   - 在根目录 `CMakeLists.txt` 中 xbase 之后添加 `add_subdirectory(xlog)`
   - 若 `XK_BUILD_TESTS` 为 ON，构建 `xlog_test` 可执行文件并链接 GTest
   - _需求：11.1、11.2、11.3、11.4_

- [ ] 2. 定义公共头文件 `xlog/logger.h`
   - 使用 `XDEF_ENUM` 定义 `xLogLevel`（Debug/Info/Warn/Error/Fatal）和 `xLogMode`（Timer/Notify/Mixed）
   - 使用 `XDEF_STRUCT` 定义 `xLoggerConf`，包含 `loop`、`path`、`mode`、`level`、`max_size`、`max_files`、`flush_interval_ms` 字段
   - 使用 `XDEF_HANDLE` 声明 `xLogger` 句柄
   - 声明生命周期函数 `xLoggerCreate(xLoggerConf conf)` / `xLoggerDestroy(xLogger logger)`
   - 声明核心日志函数 `xLoggerLog(xLogger logger, xLogLevel level, const char *fmt, ...)`
   - 声明 `xLoggerFlush(xLogger logger)`
   - 声明桥接函数 `xLoggerEnter(xLogger logger)` / `xLoggerLeave()`
   - 定义便捷宏 `XLOG_DEBUG` / `XLOG_INFO` / `XLOG_WARN` / `XLOG_ERROR` / `XLOG_FATAL`
   - 添加 Doxygen 注释，遵循 STYLE.md 规范
   - _需求：1.1、2.1、2.3、3.1–3.3（枚举值）、8.1、9.1、9.2、10.1、10.2、12.1_

- [ ] 3. 定义私有头文件 `xlog/logger_private.h`
   - 定义日志条目结构 `struct xLogEntry_`：内嵌 `xMpsc` 节点、日志级别、格式化后的消息缓冲区（`XLOG_ENTRY_BUF_SIZE`）、消息长度
   - 定义 logger 内部结构 `struct xLogger_`：`xEventLoop loop`、`FILE *fp`、`xLogMode mode`、`xLogLevel level`、MPSC 队列头尾指针（`xMpsc *head, *tail`）、pipe fd 对（Notify/Mixed 模式）、定时器 ID（Timer/Mixed 模式）、轮转相关字段（`path`、`max_size`、`max_files`、`written`）、`flush_interval_ms`
   - _需求：4.4、5.5、7.3、12.2_

- [ ] 4. 实现 logger 创建与销毁（`xlog/logger.c` 第一部分）
   - 实现 `xLoggerCreate`：校验 `loop != NULL`；分配 `struct xLogger_`；初始化 MPSC 队列；根据 `path` 以 append 模式打开文件或使用 stderr；通过 `ftell` 获取已有文件大小初始化 `written`；根据 `mode` 分支初始化定时器和/或 pipe（详见任务 5、6）；处理 `flush_interval_ms == 0` 时使用默认值 50ms
   - 实现 `xLoggerDestroy`：处理 NULL 输入（no-op）；同步 flush 队列中所有剩余日志；关闭文件句柄；取消定时器（如有）；移除 pipe fd 事件注册并关闭 pipe（如有）；释放内存
   - _需求：1.1、1.2、1.3、1.4、1.5、5.5、10.2_

- [ ] 5. 实现 Timer 模式的定时器 flush 逻辑
   - 实现定时器回调函数 `logger_timer_cb`：检查 MPSC 队列是否为空，若非空则调用内部 `logger_flush_entries` 批量写入；重新注册下一次定时器（`xEventLoopTimerAfter`）
   - 在 `xLoggerCreate` 中当 `mode == xLogMode_Timer` 或 `xLogMode_Mixed` 时注册首次定时器
   - 在 `xLoggerDestroy` 中取消定时器
   - _需求：3.1.1、3.1.2、3.1.3、3.1.4、3.3.1_

- [ ] 6. 实现 Notify 模式的 pipe 唤醒逻辑
   - 在 `xLoggerCreate` 中当 `mode == xLogMode_Notify` 或 `xLogMode_Mixed` 时：创建 pipe 对，设置非阻塞，将读端通过 `xEventAdd` 注册到事件循环上
   - 实现 pipe 读端回调函数 `logger_pipe_cb`：drain pipe（read until EAGAIN），然后调用 `logger_flush_entries` 批量写入
   - 实现内部辅助函数 `logger_notify`：向 pipe 写端写入 1 字节
   - 在 `xLoggerDestroy` 中移除 fd 事件注册并关闭 pipe 两端
   - _需求：3.2.1、3.2.2、3.2.3、3.2.4、3.3.1_

- [ ] 7. 实现核心日志记录函数 `xLoggerLog` 和便捷宏
   - 实现 `xLoggerLog`：级别过滤（低于 `logger->level` 直接返回）；在调用线程采集时间戳；在调用线程 `vsnprintf` 格式化消息到 `xLogEntry` 缓冲区（截断不崩溃）；将 entry 推入 MPSC 队列
   - 根据模式决定是否唤醒：Timer 模式不唤醒；Notify 模式每条都调用 `logger_notify`；Mixed 模式仅 `>= Error` 时调用 `logger_notify`
   - Fatal 路径：同步格式化并直接写入文件/stderr，`fflush` 后调用 `abort()`，不走异步队列
   - 确保便捷宏 `XLOG_DEBUG` 等正确展开为 `xLoggerLog` 调用
   - _需求：2.2、2.3、2.4、3.2.2、3.3.2、3.3.3、3.3.4、4.1、4.2、4.3、4.4、4.5、7.1、7.2、7.3_

- [ ] 8. 实现日志文件轮转
   - 实现内部函数 `logger_rotate`：关闭当前文件 → 删除编号最大的旧文件 → 级联重命名 `path.{i-1}` → `path.{i}` → 重命名当前文件为 `path.1` → 以 append 模式重新打开 `path` → 重置 `written = 0`
   - 在 `logger_flush_entries` 中每写完一条日志后检查 `written >= max_size`，若满足且 `max_size > 0 && max_files > 1` 则调用 `logger_rotate`
   - 轮转文件命名遵循 `<path>.1`、`<path>.2`、...、`<path>.{max_files-1}`
   - _需求：5.1.1、5.1.2、5.1.3、5.2.1–5.2.5、5.3.1、5.4.1–5.4.3_

- [ ] 9. 实现同步 Flush 和 xbase/log 桥接
   - 实现 `xLoggerFlush`：向事件循环提交 flush 请求并同步等待完成（可通过 pipe + 条件变量或 offload 机制实现）
   - 实现 `xLoggerEnter`：调用 `xLogSetCallback` 注册一个内部回调函数，该回调将 `xLog()` 的消息转发到 `xLoggerLog`；fatal 消息走同步路径
   - 实现 `xLoggerLeave`：调用 `xLogSetCallback(NULL, NULL)` 恢复默认行为
   - _需求：6.1、6.2、6.3、8.1、8.2、8.3、9.1、9.2、9.3、9.4_

- [ ] 10. 编写单元测试 `xlog/logger_test.cpp`
   - [ ] 10.1 生命周期测试：创建/销毁 logger，验证 NULL loop 返回 NULL，验证 destroy NULL 为 no-op
   - [ ] 10.2 级别过滤测试：设置 level=Warn，验证 Debug/Info 不输出，Warn/Error 正常输出
   - [ ] 10.3 Timer 模式测试：创建 Timer 模式 logger，写入日志后等待 flush_interval 后验证文件内容
   - [ ] 10.4 Notify 模式测试：创建 Notify 模式 logger，写入日志后短暂等待验证文件内容（延迟应 < 定时器间隔）
   - [ ] 10.5 Mixed 模式测试：验证 Error 级别日志立即 flush，Debug 级别日志延迟 flush
   - [ ] 10.6 文件轮转测试：设置小的 max_size，写入足够多日志触发轮转，验证轮转文件存在且命名正确
   - [ ] 10.7 stderr 输出测试：path=NULL 时验证日志输出到 stderr，不执行轮转
   - [ ] 10.8 多线程安全测试：多个线程并发写入日志，验证无崩溃且所有日志条目完整
   - [ ] 10.9 桥接测试：调用 `xLoggerEnter` 后通过 `xLog()` 写入，验证日志被重定向到 logger 文件；调用 `xLoggerLeave` 后验证恢复默认行为
   - _需求：1.1–1.5、2.1–2.3、3.1–3.3、5.1–5.4、6.1–6.3、7.1–7.3、8.1–8.3、9.1–9.4_
