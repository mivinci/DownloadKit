# 需求文档：xKit 模块文档生成

## 引言

xKit 是一套面向事件驱动、异步编程的低层 C 语言构建块集合，包含 4 个模块（xbase、xbuf、xhttp、xlog），共 17 个子功能。当前项目仅有 README.md 和 xbuf/README.md 提供了简要介绍，缺乏系统性的、面向开发者的详细文档。

本需求旨在为每个模块和每个子功能生成独立的 Markdown 文档，放置在 `docs/` 目录下，形成层次化的文档体系。每个模块对应一个子目录，目录内包含一个模块级总览文档和若干子功能文档。

### 文档目录结构

```
docs/
├── xbase/
│   ├── README.md          # xbase 模块总览
│   ├── event.md           # event.h — 跨平台事件循环
│   ├── timer.md           # timer.h — 单调定时器
│   ├── task.md            # task.h — N:M 任务模型
│   ├── socket.md          # socket.h — 异步 socket
│   ├── memory.md          # memory.h — 引用计数内存管理
│   ├── log.md             # log.h — 线程级日志回调
│   ├── backtrace.md       # backtrace.h — 平台自适应栈回溯
│   ├── error.md           # error.h — 统一错误码
│   ├── heap.md            # heap.h — 最小堆
│   ├── mpsc.md            # mpsc.h — 无锁 MPSC 队列
│   └── atomic.md          # atomic.h — 原子操作
├── xbuf/
│   ├── README.md          # xbuf 模块总览
│   ├── buf.md             # buf.h — 线性自动扩容缓冲区
│   ├── ring.md            # ring.h — 固定大小环形缓冲区
│   └── io.md              # io.h — 引用计数块链 I/O 缓冲区
├── xhttp/
│   ├── README.md          # xhttp 模块总览
│   ├── client.md          # client.h — 异步 HTTP 客户端
│   └── client_sse.md      # client_sse.c — SSE 流式客户端
└── xlog/
    ├── README.md          # xlog 模块总览
    └── logger.md          # logger.h — 高性能异步日志器
```

## 需求

### 需求 1：模块级总览文档

**用户故事：** 作为一名 C 开发者，我希望每个模块有一个总览文档，以便快速了解该模块的定位、包含哪些子功能、以及它们之间的关系。

#### 验收标准

1. WHEN 用户打开某个模块的 `README.md` THEN 文档 SHALL 包含以下章节：
   - **模块简介**：一段话概述模块的定位和核心价值
   - **设计思想**：该模块的整体设计哲学和架构决策
   - **架构图**：使用 Mermaid 语法绘制模块内各子功能之间的依赖/协作关系图
   - **子功能一览表**：表格形式列出所有头文件及其一句话描述
   - **快速上手**：一个最小可运行的代码示例，展示模块的典型用法
   - **与其他模块的关系**：说明该模块与 xKit 中其他模块的依赖和协作关系

2. IF 模块包含多个子功能 THEN 总览文档 SHALL 提供一个"如何选择"决策树或对比表，帮助用户选择合适的子功能。

3. WHEN 用户查看架构图 THEN 图中 SHALL 清晰展示子功能之间的调用关系和数据流向。

### 需求 2：子功能详细文档

**用户故事：** 作为一名 C 开发者，我希望每个子功能（头文件）有一份详细文档，以便深入理解其设计原理、API 用法和最佳实践。

#### 验收标准

1. WHEN 用户打开某个子功能的 `.md` 文档 THEN 文档 SHALL 包含以下章节：
   - **简介**：一段话描述该子功能解决什么问题
   - **设计思想**：核心设计理念（如为什么选择这种数据结构/算法）
   - **实现原理**：关键实现细节，包括内存布局、算法复杂度、线程安全性等
   - **架构图**：使用 Mermaid 语法绘制内部结构图（如内存布局、状态机、数据流等）
   - **API 参考**：列出所有公开函数/宏/类型，附带签名和简要说明
   - **使用示例**：至少一个完整的、可编译运行的代码示例
   - **使用场景**：列举 2-3 个典型使用场景
   - **最佳实践**：使用时的注意事项、常见陷阱、性能建议
   - **与其他库的比较**：与同类开源库/方案的对比（如适用）

2. IF 子功能涉及跨平台实现（如 event.h 有 kqueue/epoll/poll 三种后端）THEN 文档 SHALL 说明各平台的差异和选择逻辑。

3. IF 子功能涉及线程安全 THEN 文档 SHALL 明确标注哪些 API 是线程安全的、哪些不是，以及在多线程环境下的使用约束。

4. IF 子功能有内部依赖（如 timer.h 依赖 heap.h 和 mpsc.h）THEN 文档 SHALL 说明这些依赖关系及其作用。

5. WHEN 用户查看 API 参考 THEN 每个函数 SHALL 包含：函数签名、参数说明、返回值说明、错误码说明（如适用）、线程安全性标注。

### 需求 3：文档内容的准确性和一致性

**用户故事：** 作为一名 C 开发者，我希望文档内容与实际代码保持一致，以便信赖文档作为权威参考。

#### 验收标准

1. WHEN 文档描述某个 API THEN 函数签名 SHALL 与头文件中的声明完全一致。

2. WHEN 文档提供代码示例 THEN 示例 SHALL 使用正确的头文件包含路径（如 `#include <xbase/event.h>`）。

3. WHEN 文档描述设计原理 THEN 内容 SHALL 基于实际源码中的实现，而非臆测。

4. IF 某个 API 被标记为 `@deprecated` THEN 文档 SHALL 明确标注已废弃，并指出替代方案。

### 需求 4：文档的可读性和导航性

**用户故事：** 作为一名 C 开发者，我希望文档结构清晰、易于导航，以便快速找到所需信息。

#### 验收标准

1. WHEN 用户打开任何文档 THEN 文档 SHALL 在顶部包含面包屑导航链接（如 `[xKit](../../README.md) > [xbase](README.md) > event.h`）。

2. WHEN 用户阅读模块总览 THEN 每个子功能名称 SHALL 是指向对应详细文档的超链接。

3. WHEN 文档引用其他子功能 THEN 引用 SHALL 是可点击的相对链接。

4. WHEN 文档包含代码示例 THEN 代码块 SHALL 使用 `c` 语言标注以获得语法高亮。

5. WHEN 文档包含架构图 THEN 图 SHALL 使用 Mermaid 语法，确保在 GitHub 上可直接渲染。

### 需求 5：各模块的具体文档内容要求

**用户故事：** 作为一名 C 开发者，我希望每个模块的文档能突出其独特的设计亮点和关键技术点。

#### 验收标准

1. **xbase/event.md**：
   - SHALL 包含 kqueue/epoll/poll 三种后端的对比表
   - SHALL 解释 edge-triggered 模式的含义和使用注意事项
   - SHALL 展示事件循环的完整生命周期（创建 → 注册 → 等待 → 销毁）
   - SHALL 与 libevent、libev、libuv 进行对比

2. **xbase/timer.md**：
   - SHALL 解释 Push 模式和 Poll 模式的区别和选择依据
   - SHALL 说明内部使用最小堆的原因和时间复杂度
   - SHALL 与 timerfd、POSIX timer 进行对比

3. **xbase/task.md**：
   - SHALL 解释 N:M 任务模型的含义
   - SHALL 说明线程池的工作窃取/调度策略
   - SHALL 与 pthread、C11 threads、GCD 进行对比

4. **xbase/socket.md**：
   - SHALL 展示与 xEventLoop 的集成方式
   - SHALL 解释 idle-timeout 的实现机制
   - SHALL 与 POSIX socket API 进行对比

5. **xbase/memory.md**：
   - SHALL 解释 vtable 驱动的生命周期管理
   - SHALL 展示 XMALLOC/XMALLOCEX 宏的展开过程
   - SHALL 与 C++ RAII、Objective-C ARC 进行对比

6. **xbuf/buf.md**：
   - SHALL 展示内存布局图（flexible array member）
   - SHALL 解释 2x 扩容策略和 compact 机制
   - SHALL 与 Go bytes.Buffer、Rust Vec<u8> 进行对比

7. **xbuf/ring.md**：
   - SHALL 展示环形缓冲区的内存布局和读写指针
   - SHALL 解释 power-of-2 掩码索引的优化原理
   - SHALL 与 Linux kfifo 进行对比

8. **xbuf/io.md**：
   - SHALL 展示 block-chain 架构图
   - SHALL 解释零拷贝 split/append 的实现原理
   - SHALL 解释 Treiber stack 无锁 freelist 的工作方式
   - SHALL 与 brpc IOBuf、Netty ByteBuf 进行对比

9. **xhttp/client.md**：
   - SHALL 展示 libcurl multi-socket + xEventLoop 的集成架构图
   - SHALL 说明请求的完整生命周期
   - SHALL 与 libcurl easy API、cpp-httplib 进行对比

10. **xhttp/client_sse.md**：
    - SHALL 解释 W3C SSE 规范的解析逻辑
    - SHALL 展示 SSE 流的数据流图
    - SHALL 提供 LLM API 调用的完整示例

11. **xlog/logger.md**：
    - SHALL 解释 Timer/Notify/Mixed 三种模式的区别
    - SHALL 展示 MPSC 队列 + 事件循环的异步刷写架构
    - SHALL 解释日志轮转的实现机制
    - SHALL 与 spdlog、zlog、log4c 进行对比

12. **xbase 其余子功能**（log.md、backtrace.md、error.md、heap.md、mpsc.md、atomic.md、time.md）：
    - SHALL 各自包含完整的标准章节结构
    - SHALL 重点说明其在 xKit 内部被哪些上层模块使用

### 需求 6：文档语言

**用户故事：** 作为一名开发者，我希望文档使用英文撰写，以便国际化受众阅读。

#### 验收标准

1. WHEN 生成任何文档 THEN 文档正文 SHALL 使用英文撰写。
2. WHEN 文档包含代码注释 THEN 注释 SHALL 使用英文。
3. WHEN 文档包含 Mermaid 图 THEN 图中的标签 SHALL 使用英文。
