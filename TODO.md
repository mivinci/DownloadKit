# moo TODO

## 现有模块梳理

| 层级 | 模块 | 职责 |
| ------ | ------ | ------ |
| **xbase** | event (kqueue/epoll/poll) | 跨平台事件循环 |
| | timer / heap | 定时器 + 最小堆 |
| | task / mpsc | 线程池 + 无锁队列 |
| | socket | 异步 socket 抽象 |
| | memory / log / error / backtrace | 基础工具 |
| **xhttp** | client | 基于 libcurl multi + xEventLoop 的异步 HTTP 客户端 |

整体架构是 **事件循环 → 异步 I/O → 上层协议**，风格很像 libuv 但更轻量、纯 C、接口更精简。基于这个定位，以下几个方向比较自然：

---

## 1. xnet — 异步网络层（DNS + TCP + UDP + TLS）

在 `xSocket` 之上封装一层完整的网络抽象，涵盖 DNS 解析、TCP/UDP 连接管理和 TLS 加密：

### DNS 解析

- 基于 `xEventLoopSubmit` 把 `getaddrinfo` offload 到线程池（简单方案）
- 或者自己实现 UDP DNS 协议直接走 `xSocket`（高级方案，类似 c-ares）
- 提供 `xDnsResolve(loop, hostname, callback, arg)` 这样的接口

### TCP 连接器 / 监听器

- **xTcpConnect** — 非阻塞 connect + 超时 + DNS 解析，一步到位
- **xTcpListener** — bind + listen + accept 循环，每个新连接回调一个 `xSocket`
- 处理 `SO_KEEPALIVE`、`TCP_NODELAY` 等常见选项

### UDP

- **xUdpSocket** — 异步 UDP 收发，集成到事件循环（`EPOLLIN` / `EVFILT_READ`）
- 支持 `sendto` / `recvfrom` 回调模型，适配无连接场景
- 可选 connected UDP（`connect` 后用 `send`/`recv`），减少每次发送的地址查找开销
- 为上层协议（DNS、QUIC、自定义 RPC 等）提供基础传输

### TLS 封装

- 基于 OpenSSL / BoringSSL / mbedTLS 的 BIO 接口
- 与 `xSocket` 集成，提供 `xSslSocket`，握手过程完全异步
- `xhttp` 模块目前依赖 libcurl 自带的 TLS，但如果未来要做 HTTP server 或其他协议就需要独立的 TLS 层

DNS → TCP/UDP → TLS 是建立网络连接的完整链路，放在同一模块中可以让上层协议（如 HTTPS、QUIC）直接调用 `xnet` 一步到位。

## 2. xbuf — 缓冲区模块 ✅

异步 socket 编程中，读写缓冲区管理是绑定的需求。已实现三种缓冲区：

- `xBuffer`（`buf.h`）— 线性自动扩容字节缓冲区（类似 Go 的 `bytes.Buffer`），2x 扩容策略
- `xRingBuffer`（`ring.h`）— 固定大小环形缓冲区，power-of-2 掩码索引，适合流式协议解析
- `xIOBuffer`（`io.h`）— 引用计数 block-chain I/O 缓冲区（brpc IOBuf 风格），支持零拷贝 split/cut、scatter-gather I/O

这样 `xSocket` 的用户就不用每次自己管理 `read`/`write` 的 partial 问题了。

## 3. xhttp/server — 异步 HTTP Server ✅

现在只有 HTTP client，加一个 server 端会让整个库的实用性大幅提升：

- 基于 `xSocket` + `xTcpListener` 接受连接
- 内置 HTTP/1.1 请求解析（可以用 llhttp 或自己写一个轻量 parser）
- 路由 + handler 回调模型
- 接口风格类似：`xHttpServerCreate(loop)` → `xHttpServerRoute(server, "GET /path", handler)` → `xHttpServerListen(server, ":8080")`

## 4. xlog 增强 — 异步日志 ✅

基于 MPSC 无锁队列 + 事件循环线程消费的异步日志模块，已实现：

- 异步写入：调用线程格式化日志后入队，事件循环线程负责落盘，无需额外线程池或专用写线程
- 三种刷新模式：Timer（定时器周期刷新）、Notify（pipe 通知立即刷新）、Mixed（两者结合）
- 日志级别过滤（Debug/Info/Warn/Error/Fatal）
- 文件轮转（max_size + max_files）
- 全局无锁 freelist 复用 entry，减少热路径上的 malloc
- 线程局部 logger 上下文（xLoggerEnter/xLoggerLeave）+ 便捷宏（XLOG_DEBUG 等）
- 同步 flush 支持（xLoggerFlush）
- Fatal 级别同步写入后 abort()

## 5. xsignal — 信号处理 ✅

事件循环通常需要优雅处理 SIGINT / SIGTERM 等信号：

- 用 `signalfd`（Linux）或 `EVFILT_SIGNAL`（kqueue）集成到事件循环
- 或者经典的 self-pipe trick 作为 fallback
- 提供 `xSignalWatch(loop, signo, callback, arg)` 接口

## 6. 内存分配优化

项目中存在大量零碎的 malloc/free，考虑引入对象池（pool）或 arena 分配器，减少堆碎片和系统调用开销。
