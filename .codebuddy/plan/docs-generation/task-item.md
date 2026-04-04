# 实施计划

- [ ] 1. 创建 docs 目录结构和文档模板
   - 创建 `docs/xbase/`、`docs/xbuf/`、`docs/xhttp/`、`docs/xlog/` 四个目录
   - 在每个目录下创建空的 `README.md` 和各子功能 `.md` 文件（共 21 个文件）
   - 每个文件预填充统一的章节骨架（面包屑导航、标准章节标题），确保导航链接正确
   - _需求：4.1、4.2、4.3、4.5_

- [ ] 2. 编写 xbase 模块总览文档 `docs/xbase/README.md`
   - 阅读 `xbase/` 下所有头文件和源文件，提取模块定位和设计哲学
   - 编写模块简介、设计思想、与其他模块的关系章节
   - 绘制 Mermaid 架构图展示 11 个子功能之间的依赖关系（event → timer/socket/task, timer → heap/mpsc 等）
   - 编写子功能一览表（表格含超链接）和"如何选择"对比表
   - 编写快速上手代码示例（展示 event loop + timer 的典型用法）
   - _需求：1.1、1.2、1.3、4.2、6.1_

- [ ] 3. 编写 xbase 核心子功能文档：event.md、timer.md、task.md、socket.md
   - **event.md**：阅读 `event.h`、`event_private.h`、`event_kqueue.c`、`event_epoll.c`、`event_poll.c`，编写 kqueue/epoll/poll 对比表、edge-triggered 说明、事件循环生命周期图、与 libevent/libev/libuv 对比
   - **timer.md**：阅读 `timer.h`、`timer.c`，编写 Push/Poll 模式对比、最小堆原理和时间复杂度分析、与 timerfd/POSIX timer 对比
   - **task.md**：阅读 `task.h`、`task.c`，编写 N:M 任务模型说明、调度策略分析、与 pthread/C11 threads/GCD 对比
   - **socket.md**：阅读 `socket.h`、`socket.c`，编写与 xEventLoop 集成方式、idle-timeout 机制、与 POSIX socket API 对比
   - 每个文档包含完整标准章节：简介、设计思想、实现原理、Mermaid 架构图、API 参考（含签名/参数/返回值/线程安全性）、使用示例、使用场景、最佳实践、对比
   - _需求：2.1、2.2、2.3、2.4、2.5、3.1、3.2、3.3、5.1、5.2、5.3、5.4、6.1_

- [ ] 4. 编写 xbase 基础设施子功能文档：memory.md、error.md
   - **memory.md**：阅读 `memory.h`、`memory.c`，编写 vtable 驱动生命周期管理、XMALLOC/XMALLOCEX 宏展开过程、与 C++ RAII/Objective-C ARC 对比
   - **error.md**：阅读 `error.h`，编写统一错误码体系说明、错误码枚举表、使用示例
   - 每个文档包含完整标准章节
   - _需求：2.1、2.5、3.1、5.5、5.12、6.1_

- [ ] 5. 编写 xbase 数据结构与并发子功能文档：heap.md、mpsc.md、atomic.md
   - **heap.md**：阅读 `heap.h`、`heap.c`，编写最小堆实现原理、时间复杂度分析、在 timer 模块中的应用说明
   - **mpsc.md**：阅读 `mpsc.h`、`mpsc.c`，编写无锁 MPSC 队列原理、内存序分析、在 timer/xlog 中的应用说明
   - **atomic.md**：阅读 `atomic.h`，编写原子操作封装说明、各平台实现差异、在 mpsc/memory 中的应用说明
   - 每个文档包含完整标准章节，重点说明被哪些上层模块使用
   - _需求：2.1、2.3、2.4、2.5、3.1、5.12、6.1_

- [ ] 6. 编写 xbase 辅助子功能文档：log.md、backtrace.md
   - **log.md**：阅读 `log.h`、`log.c`，编写线程级日志回调机制、日志级别说明、使用示例
   - **backtrace.md**：阅读 `backtrace.h`、`backtrace.c`，编写平台自适应栈回溯原理（execinfo/libunwind/CaptureStackBackTrace）、使用示例
   - 每个文档包含完整标准章节，重点说明被哪些上层模块使用
   - _需求：2.1、2.2、2.5、3.1、5.12、6.1_

- [ ] 7. 编写 xbuf 模块总览文档和子功能文档
   - **README.md**：编写模块简介、设计思想、Mermaid 架构图（buf/ring/io 三者关系）、子功能一览表、"如何选择"对比表、快速上手示例
   - **buf.md**：阅读 `buf.h`、`buf.c`，编写 flexible array member 内存布局图、2x 扩容策略和 compact 机制、与 Go bytes.Buffer/Rust Vec<u8> 对比
   - **ring.md**：阅读 `ring.h`、`ring.c`，编写环形缓冲区内存布局和读写指针图、power-of-2 掩码索引优化原理、与 Linux kfifo 对比
   - **io.md**：阅读 `io.h`、`io.c`，编写 block-chain 架构图、零拷贝 split/append 原理、Treiber stack 无锁 freelist 说明、与 brpc IOBuf/Netty ByteBuf 对比
   - 每个文档包含完整标准章节
   - _需求：1.1、1.2、1.3、2.1、2.3、2.5、3.1、5.6、5.7、5.8、6.1_

- [ ] 8. 编写 xhttp 模块总览文档和子功能文档
   - **README.md**：编写模块简介、设计思想、Mermaid 架构图（client + SSE 与 xbase/xbuf 的关系）、子功能一览表、快速上手示例
   - **client.md**：阅读 `client.h`、`client.c`，编写 libcurl multi-socket + xEventLoop 集成架构图、请求完整生命周期、与 libcurl easy API/cpp-httplib 对比
   - **client_sse.md**：阅读 `client_sse.c`，编写 W3C SSE 规范解析逻辑、SSE 流数据流图、LLM API 调用完整示例
   - 每个文档包含完整标准章节
   - _需求：1.1、1.2、1.3、2.1、2.4、2.5、3.1、5.9、5.10、6.1_

- [ ] 9. 编写 xlog 模块总览文档和子功能文档
   - **README.md**：编写模块简介、设计思想、Mermaid 架构图（logger 与 xbase event/mpsc 的关系）、快速上手示例
   - **logger.md**：阅读 `logger.h`、`logger.c`，编写 Timer/Notify/Mixed 三种模式对比、MPSC 队列 + 事件循环异步刷写架构图、日志轮转机制、与 spdlog/zlog/log4c 对比
   - 每个文档包含完整标准章节
   - _需求：1.1、1.2、1.3、2.1、2.3、2.4、2.5、3.1、5.11、6.1_

- [ ] 10. 全局审查与交叉链接校验
   - 检查所有 21 个文档的面包屑导航链接是否正确
   - 检查所有模块总览中的子功能超链接是否指向正确的 `.md` 文件
   - 检查所有文档间的交叉引用链接是否可点击
   - 检查所有 API 签名是否与头文件中的声明完全一致
   - 检查所有代码示例是否使用正确的 `#include` 路径和 `c` 语言标注
   - 检查所有 Mermaid 图是否语法正确、可在 GitHub 上渲染
   - _需求：3.1、3.2、3.3、3.4、4.1、4.2、4.3、4.4、4.5_
