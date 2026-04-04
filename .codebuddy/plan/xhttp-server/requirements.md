# 需求文档：xhttp/server — 异步 HTTP Server

## 引言

xhttp 模块目前仅包含基于 libcurl 的异步 HTTP 客户端。本需求旨在为 xhttp 模块新增 **server 端**，提供一个轻量、高性能的异步 HTTP/1.1 服务器。

服务器基于 xbase 的 `xSocket`（异步 socket 抽象）和 `xEventLoop`（事件循环）构建，使用 `xIOBuffer`（零拷贝 block-chain 缓冲区）进行读写，并集成 **llhttp** 作为 HTTP 解析器。API 风格与现有 xhttp/client 保持一致（opaque handle + callback 模型），所有回调在事件循环线程上分发，无需用户侧加锁。

### 技术栈依赖

| 组件 | 来源 | 用途 |
| --- | --- | --- |
| `xEventLoop` | xbase/event.h | 事件循环，驱动所有 I/O |
| `xSocket` | xbase/socket.h | 异步 socket 抽象（非阻塞 + 事件注册 + 超时） |
| `xIOBuffer` | xbuf/io.h | 零拷贝读写缓冲区（block-chain + writev） |
| `llhttp` | 第三方（FindLlhttp.cmake） | HTTP/1.1 请求解析 |

---

## 需求

### 需求 1：服务器生命周期管理

**用户故事：** 作为一名 C 开发者，我希望能够创建、启动和销毁一个 HTTP 服务器实例，以便在事件循环中提供 HTTP 服务。

#### 验收标准

1. WHEN 用户调用 `xHttpServerCreate(loop)` THEN 系统 SHALL 返回一个绑定到指定事件循环的 opaque `xHttpServer` handle，失败时返回 NULL。
2. WHEN 用户调用 `xHttpServerListen(server, host, port)` THEN 系统 SHALL 在指定地址上创建监听 socket（`SO_REUSEADDR`），开始 accept 新连接。
3. WHEN 用户调用 `xHttpServerDestroy(server)` THEN 系统 SHALL 关闭监听 socket，断开所有活跃连接（触发各连接的清理回调），释放所有资源。
4. IF 监听地址已被占用 THEN `xHttpServerListen` SHALL 返回错误码（`xErrno`），不会崩溃。
5. WHEN `xHttpServerDestroy` 被调用时仍有活跃连接 THEN 系统 SHALL 优雅关闭所有连接后再释放服务器资源。

---

### 需求 2：连接管理

**用户故事：** 作为一名 C 开发者，我希望服务器能自动管理 TCP 连接的接受、保活和关闭，以便我只需关注业务逻辑。

#### 验收标准

1. WHEN 监听 socket 上有新连接到达 THEN 系统 SHALL 调用 `accept()` 并为每个新连接创建一个 `xHttpConn` 内部结构（包含 `xSocket`、读写 `xIOBuffer`、llhttp 解析器状态）。
2. WHEN 请求头中包含 `Connection: keep-alive`（或 HTTP/1.1 默认行为） THEN 系统 SHALL 在响应发送完成后保持连接，等待下一个请求。
3. WHEN 请求头中包含 `Connection: close` THEN 系统 SHALL 在响应发送完成后关闭连接。
4. IF 连接在 `idle_timeout_ms` 时间内没有收到任何数据 THEN 系统 SHALL 自动关闭该连接（利用 `xSocketSetTimeout`）。
5. WHEN 客户端主动关闭连接（read 返回 0） THEN 系统 SHALL 清理该连接的所有资源。
6. IF accept 返回 `EMFILE`/`ENFILE`（fd 耗尽） THEN 系统 SHALL 记录警告日志并继续运行，不崩溃。

---

### 需求 3：HTTP 请求解析

**用户故事：** 作为一名 C 开发者，我希望服务器能正确解析 HTTP/1.1 请求，以便我能获取到方法、路径、头部和请求体。

#### 验收标准

1. WHEN socket 可读时 THEN 系统 SHALL 使用 `xIOBufferReadFd` 读取数据，并将数据喂给 llhttp 解析器。
2. WHEN llhttp 解析完成一个完整请求 THEN 系统 SHALL 构造一个 `xHttpRequest` 结构（包含 method、url、headers、body 指针）并触发路由匹配。
3. WHEN 请求包含 `Content-Length` 头 THEN 系统 SHALL 等待接收完整 body 后再触发 handler。
4. WHEN 请求包含 `Transfer-Encoding: chunked` THEN 系统 SHALL 正确解码分块传输并拼接 body。
5. IF llhttp 报告解析错误 THEN 系统 SHALL 向客户端发送 400 Bad Request 响应并关闭连接。
6. IF 请求头总大小超过可配置的 `max_header_size`（默认 8KB） THEN 系统 SHALL 返回 431 Request Header Fields Too Large 并关闭连接。
7. IF 请求体大小超过可配置的 `max_body_size`（默认 1MB） THEN 系统 SHALL 返回 413 Content Too Large 并关闭连接。

---

### 需求 4：路由与请求分发

**用户故事：** 作为一名 C 开发者，我希望能注册路由规则（方法 + 路径 → handler），以便不同的 URL 路径由不同的处理函数处理。

#### 验收标准

1. WHEN 用户调用 `xHttpServerRoute(server, method, path, handler, arg)` THEN 系统 SHALL 注册一条路由规则，将匹配的请求分发到指定 handler。
2. WHEN 收到请求且路径精确匹配某条路由 THEN 系统 SHALL 调用对应的 handler 回调，传入 `xHttpRequest` 和 `xHttpResponseWriter`。
3. IF 没有路由匹配当前请求 THEN 系统 SHALL 自动返回 404 Not Found 响应。
4. IF 路由路径匹配但 HTTP 方法不匹配 THEN 系统 SHALL 自动返回 405 Method Not Allowed 响应。
5. WHEN 用户注册多条路由 THEN 系统 SHALL 按注册顺序进行匹配（先注册优先）。
6. WHEN 用户调用 `xHttpServerRoute` 且 method 为 NULL THEN 系统 SHALL 将该路由注册为匹配所有 HTTP 方法。

---

### 需求 5：响应构建与发送

**用户故事：** 作为一名 C 开发者，我希望能方便地构建和发送 HTTP 响应（状态码、头部、body），以便快速实现业务逻辑。

#### 验收标准

1. WHEN handler 被调用时 THEN 系统 SHALL 提供一个 `xHttpResponseWriter` 对象，用于构建响应。
2. WHEN 用户调用 `xHttpResponseSetStatus(writer, code)` THEN 系统 SHALL 设置响应状态码。
3. WHEN 用户调用 `xHttpResponseSetHeader(writer, key, value)` THEN 系统 SHALL 添加一个响应头。
4. WHEN 用户调用 `xHttpResponseSend(writer, body, body_len)` THEN 系统 SHALL 将状态行 + 头部 + body 序列化到 `xIOBuffer`，并通过 `xIOBufferWriteFd` / `writev` 发送。
5. IF 用户在 handler 中未调用任何响应方法 THEN 系统 SHALL 在 handler 返回后自动发送 200 OK（空 body）。
6. WHEN 写缓冲区满（`writev` 返回 `EAGAIN`） THEN 系统 SHALL 将剩余数据保留在写 `xIOBuffer` 中，注册 `xEvent_Write`，待 socket 可写时继续发送（背压控制）。
7. WHEN 响应发送完成 THEN 系统 SHALL 根据 keep-alive 策略决定是保持连接还是关闭连接。

---

### 需求 6：服务器配置

**用户故事：** 作为一名 C 开发者，我希望能配置服务器的关键参数，以便根据不同场景调优。

#### 验收标准

1. WHEN 用户在 `xHttpServerListen` 之前调用配置接口 THEN 系统 SHALL 接受以下可配置参数：
   - `idle_timeout_ms`：连接空闲超时（默认 60000ms）
   - `max_header_size`：请求头最大字节数（默认 8192）
   - `max_body_size`：请求体最大字节数（默认 1048576）
2. IF 用户未设置某项配置 THEN 系统 SHALL 使用上述默认值。
3. WHEN 配置值不合法（如 timeout 为负数） THEN 系统 SHALL 返回 `xErrno_InvalidArg`。

---

### 需求 7：边界情况与健壮性

**用户故事：** 作为一名 C 开发者，我希望服务器在各种异常情况下都能稳定运行，不会崩溃或泄漏资源。

#### 验收标准

1. IF 传入 NULL 参数（server、writer 等） THEN 所有公开 API SHALL 安全返回错误码或 no-op，不崩溃。
2. WHEN 客户端发送半截请求后断开 THEN 系统 SHALL 清理该连接的解析器状态和缓冲区，无内存泄漏。
3. WHEN 服务器在高并发下运行 THEN 系统 SHALL 不存在 use-after-free（利用 xEventLoop 的 deferred source deletion 机制）。
4. WHEN handler 回调中发生 panic（如 SIGSEGV） THEN 系统 SHALL 不影响其他连接的处理（单连接隔离）。
5. IF `xIOBlockPool` 未预热 THEN 系统 SHALL 仍能正常工作（按需 malloc），但建议用户在启动时调用 `xIOBlockPoolWarmup`。

---

### 需求 8：构建集成

**用户故事：** 作为一名 C 开发者，我希望 xhttp/server 能无缝集成到现有的 CMake 构建系统中。

#### 验收标准

1. WHEN 系统安装了 llhttp THEN `find_package(Llhttp REQUIRED)` SHALL 通过 `FindLlhttp.cmake` 成功找到库。
2. WHEN 构建 xhttp 模块 THEN CMakeLists.txt SHALL 将 server 源文件编译进 xhttp 库，并链接 `Llhttp::Llhttp`。
3. IF 系统未安装 llhttp THEN 构建 SHALL 报告清晰的错误信息，指导用户安装。
4. WHEN 构建完成 THEN server 的公开头文件 SHALL 可通过 `#include <xhttp/server.h>` 引用。
