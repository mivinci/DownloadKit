# xKit TODO

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

## 1. xdns — 异步 DNS 解析

目前 `xSocket` 直接操作 fd，但实际使用中 DNS 解析是第一个阻塞点。可以做一个轻量的异步 DNS 模块：

- 基于 `xEventLoopSubmit` 把 `getaddrinfo` offload 到线程池（简单方案）
- 或者自己实现 UDP DNS 协议直接走 `xSocket`（高级方案，类似 c-ares）
- 提供 `xDnsResolve(loop, hostname, callback, arg)` 这样的接口

这个模块几乎是做 TCP 客户端连接前的必备环节。

## 2. xbuf — 零拷贝缓冲区 / 环形缓冲区

异步 socket 编程中，读写缓冲区管理是绑定的需求。可以提供：

- `xBuf` — 自动扩容的字节缓冲区（类似 Go 的 `bytes.Buffer`）
- 或 `xRingBuf` — 固定大小环形缓冲区，适合流式协议解析
- 支持 `readv`/`writev` 的 scatter-gather I/O 接口

这样 `xSocket` 的用户就不用每次自己管理 `read`/`write` 的 partial 问题了。

## 3. xtcp — 异步 TCP 连接器 / 监听器

在 `xSocket` 之上再封装一层，处理 TCP 特有的流程：

- **xTcpConnect** — 非阻塞 connect + 超时 + DNS 解析，一步到位
- **xTcpListener** — bind + listen + accept 循环，每个新连接回调一个 `xSocket`
- 处理 `SO_KEEPALIVE`、`TCP_NODELAY` 等常见选项

这是从 raw socket 到可用网络服务之间缺失的一层。

## 4. xssl — TLS 封装

如果要做真正可用的网络库，TLS 是绕不开的：

- 基于 OpenSSL / BoringSSL / mbedTLS 的 BIO 接口
- 与 `xSocket` 集成，提供 `xSslSocket`，握手过程完全异步
- `xhttp` 模块目前依赖 libcurl 自带的 TLS，但如果未来要做 HTTP server 或其他协议就需要独立的 TLS 层

## 5. xhttp/server — 异步 HTTP Server

现在只有 HTTP client，加一个 server 端会让整个库的实用性大幅提升：

- 基于 `xSocket` + `xTcpListener` 接受连接
- 内置 HTTP/1.1 请求解析（可以用 llhttp 或自己写一个轻量 parser）
- 路由 + handler 回调模型
- 接口风格类似：`xHttpServerCreate(loop)` → `xHttpServerRoute(server, "GET", "/path", handler)` → `xHttpServerListen(server, ":8080")`

## 6. xlog 增强 — 异步日志

现有的 `log` 模块看起来比较基础。可以增强为：

- 异步写入（通过 `xEventLoopSubmit` offload 到线程池，或用 MPSC 队列 + 专用写线程）
- 支持日志级别过滤、格式化、文件轮转
- 这个跟事件循环的集成会很自然

## 7. xsignal — 信号处理 ✅

事件循环通常需要优雅处理 SIGINT / SIGTERM 等信号：

- 用 `signalfd`（Linux）或 `EVFILT_SIGNAL`（kqueue）集成到事件循环
- 或者经典的 self-pipe trick 作为 fallback
- 提供 `xSignalWatch(loop, signo, callback, arg)` 接口

---

## 优先级建议

1. **xbuf**（缓冲区）— 几乎是做任何协议解析的前置条件，工作量小，收益高
2. **xtcp**（TCP 连接器/监听器）— 让 socket 真正可用，是 server 端的基础
3. **xsignal**（信号处理）— 工作量很小，但对 daemon 进程是刚需 ✅
4. **xdns**（异步 DNS）— 客户端连接的必备环节
5. **xhttp/server** — 杀手级功能，但依赖前面几个模块
